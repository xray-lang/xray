/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_pass_cfg.c - XIR CFG and control flow optimization passes
 *
 * KEY CONCEPT:
 *   Copy propagation, branch simplification, unreachable block elimination,
 *   phi simplification, CFG rebuild, critical edge splitting, and
 *   representation selection passes.
 */

#include "xir_pass_internal.h"
#include "xir_looptree.h"
#include "../base/xchecks.h"

/* ========== Copy Propagation ========== */

/*
 * Replace uses of MOV-defined vregs with their original source,
 * collapsing copy chains. This exposes dead MOVs for subsequent DCE.
 *
 * Algorithm:
 *   1. Build copy_of[v] map: for each vreg v defined by MOV, record src.
 *   2. Chase copy chains to find the root (non-MOV origin).
 *   3. Rewrite all instruction args, phi args, and terminator args
 *      to use the root instead of intermediate copies.
 *   4. Repeat until no more changes.
 */
XirPassChange xir_pass_copy_prop(XirFunc *func) {
    if (!func || func->nvreg == 0) return xir_pass_no_change();

    uint32_t nv = func->nvreg;
    XirRef *copy_of = (XirRef *)xr_malloc(nv * sizeof(XirRef));
    if (!copy_of) return xir_pass_no_change();

    uint32_t iters = 0;
    bool changed = true;
    while (changed) {
        changed = false;
        iters++;

        // Initialize: every vreg maps to itself
        for (uint32_t i = 0; i < nv; i++)
            copy_of[i] = XIR_REF(XIR_REF_VREG, i);

        // Pass 1: collect MOV definitions and Phi-as-copy
        for (uint32_t bi = 0; bi < func->nblk; bi++) {
            XirBlock *blk = func->blocks[bi];

            // Phi nodes: if all non-self args resolve to same root, treat as copy
            for (XirPhi *phi = blk->phis; phi; phi = phi->next) {
                if (!xir_ref_is_vreg(phi->dst)) continue;
                XirRef common = XIR_NONE;
                bool all_same = true;
                for (uint32_t p = 0; p < phi->narg && all_same; p++) {
                    XirRef arg = phi->args[p];
                    if (xir_ref_is_none(arg)) continue;
                    if (xir_ref_is_vreg(arg) && arg == phi->dst) continue;
                    // Resolve through existing copy chains
                    if (xir_ref_is_vreg(arg)) {
                        uint32_t idx = XIR_REF_INDEX(arg);
                        if (idx < nv) arg = copy_of[idx];
                    }
                    if (xir_ref_is_none(common)) {
                        common = arg;
                    } else if (common != arg) {
                        all_same = false;
                    }
                }
                if (all_same && !xir_ref_is_none(common)) {
                    uint32_t dst = XIR_REF_INDEX(phi->dst);
                    // Only propagate if reps match — crossing rep
                    // boundaries would break typed instructions
                    // (e.g. FADD expects F64 args).
                    bool rep_ok = true;
                    if (dst < nv && xir_ref_is_vreg(common)) {
                        uint32_t ci = XIR_REF_INDEX(common);
                        if (ci < nv && func->vregs[dst].rep != func->vregs[ci].rep)
                            rep_ok = false;
                    }
                    if (dst < nv && rep_ok) copy_of[dst] = common;
                }
            }

            for (uint32_t i = 0; i < blk->nins; i++) {
                XirIns *ins = &blk->ins[i];
                if (!xir_op_is_copy(ins->op)) continue;
                if (!xir_ref_is_vreg(ins->dst)) continue;
                if (!xir_ref_is_vreg(ins->args[0])) continue;
                uint32_t dst = XIR_REF_INDEX(ins->dst);
                uint32_t src = XIR_REF_INDEX(ins->args[0]);
                // Only propagate if reps match — crossing rep
                // boundaries would break typed instructions.
                if (dst < nv && src < nv &&
                    func->vregs[dst].rep == func->vregs[src].rep)
                    copy_of[dst] = ins->args[0];
            }
        }

        // Chase chains: copy_of[v] → copy_of[copy_of[v]] → ...
        for (uint32_t i = 0; i < nv; i++) {
            XirRef r = copy_of[i];
            uint32_t limit = nv;
            while (xir_ref_is_vreg(r) && limit-- > 0) {
                uint32_t idx = XIR_REF_INDEX(r);
                if (idx >= nv) break;
                if (copy_of[idx] == r) break;
                r = copy_of[idx];
            }
            copy_of[i] = r;
        }

        // Propagate bc_slot: when vreg i is replaced by root,
        // root inherits i's bc_slot ONLY if root has none yet.
        // Never overwrite an existing valid bc_slot — the builder's
        // relocation logic in builder_set_slot already tracks where
        // the value lives as bytecode registers are reused. Overwriting
        // would cause OSR to load from stale bytecode registers.
        for (uint32_t i = 0; i < nv; i++) {
            XirRef root = copy_of[i];
            if (!xir_ref_is_vreg(root)) continue;
            uint32_t ri = XIR_REF_INDEX(root);
            if (ri >= nv || ri == i) continue;
            int16_t src_slot = func->vregs[i].bc_slot;
            if (src_slot < 0) continue;
            if (func->vregs[ri].bc_slot < 0) {
                func->vregs[ri].bc_slot = src_slot;
            }
        }

        // Propagate xrtype: when vreg i is replaced by root,
        // root should inherit i's precise static type if it has none.
        for (uint32_t i = 0; i < nv; i++) {
            XirRef root = copy_of[i];
            if (!xir_ref_is_vreg(root)) continue;
            uint32_t ri = XIR_REF_INDEX(root);
            if (ri >= nv || ri == i) continue;
            if (func->vregs[ri].xrtype == NULL && func->vregs[i].xrtype != NULL)
                func->vregs[ri].xrtype = func->vregs[i].xrtype;
        }

        // Propagate vtag and heap_type: v_copy = MOV v_root means they
        // hold the same SSA value. If the copy has a more precise vtag
        // (e.g., type_prop narrowed it via a guard), upgrade the root.
        // Safe because same value ⇒ same runtime type.
        // Only upgrade TAGGED→concrete; never downgrade or cross-change.
        // Must check rep compatibility: don't set VTAG_I64 on a F64 vreg.
        for (uint32_t i = 0; i < nv; i++) {
            XirRef root = copy_of[i];
            if (!xir_ref_is_vreg(root)) continue;
            uint32_t ri = XIR_REF_INDEX(root);
            if (ri >= nv || ri == i) continue;
            uint8_t src_vtag = type_kind_to_vtag(xir_ref_ctype(func, XIR_REF(XIR_REF_VREG, i)).kind);
            uint8_t dst_vtag = type_kind_to_vtag(xir_ref_ctype(func, root).kind);
            if (dst_vtag == VTAG_TAGGED && src_vtag != VTAG_TAGGED) {
                // Check rep compatibility before upgrading
                uint8_t rp = func->vregs[ri].rep;
                bool compat = true;
                if (src_vtag == VTAG_I64 || src_vtag == VTAG_BOOL)
                    compat = (rp == XR_REP_I64 || rp == XR_REP_PTR || rp == XR_REP_TAGGED);
                else if (src_vtag == VTAG_F64)
                    compat = (rp == XR_REP_F64 || rp == XR_REP_TAGGED);
                else if (src_vtag == VTAG_PTR)
                    compat = (rp == XR_REP_PTR || rp == XR_REP_I64 || rp == XR_REP_TAGGED);
                if (compat) {
                    XirIns *def = func->vregs[ri].def;
                    if (def && def->ctype.kind == XIR_TK_UNKNOWN)
                        def->ctype.kind = vtag_to_type_kind(src_vtag);
                }
            }
            if (func->vregs[ri].heap_type == 0 && func->vregs[i].heap_type != 0) {
                func->vregs[ri].heap_type = func->vregs[i].heap_type;
                XirIns *def = func->vregs[ri].def;
                if (def && def->ctype.heap_cid == 0)
                    def->ctype.heap_cid = func->vregs[i].heap_type;
            }
        }

        // Pass 2: rewrite uses
        for (uint32_t bi = 0; bi < func->nblk; bi++) {
            XirBlock *blk = func->blocks[bi];

            // Instruction args
            for (uint32_t i = 0; i < blk->nins; i++) {
                XirIns *ins = &blk->ins[i];
                for (int a = 0; a < 2; a++) {
                    if (!xir_ref_is_vreg(ins->args[a])) continue;
                    uint32_t idx = XIR_REF_INDEX(ins->args[a]);
                    if (idx >= nv) continue;
                    XirRef root = copy_of[idx];
                    if (root != ins->args[a]) {
                        ins->args[a] = root;
                        changed = true;
                    }
                }
            }

            // Phi args
            for (XirPhi *phi = blk->phis; phi; phi = phi->next) {
                for (uint32_t p = 0; p < phi->narg; p++) {
                    if (!xir_ref_is_vreg(phi->args[p])) continue;
                    uint32_t idx = XIR_REF_INDEX(phi->args[p]);
                    if (idx >= nv) continue;
                    XirRef root = copy_of[idx];
                    if (root != phi->args[p]) {
                        phi->args[p] = root;
                        changed = true;
                    }
                }
            }

            // Terminator arg
            if (xir_ref_is_vreg(blk->jmp.arg)) {
                uint32_t idx = XIR_REF_INDEX(blk->jmp.arg);
                if (idx < nv) {
                    XirRef root = copy_of[idx];
                    if (root != blk->jmp.arg) {
                        blk->jmp.arg = root;
                        changed = true;
                    }
                }
            }
        }

        // Deopt info slot refs: keep them in sync with copy chains
        // so build_runtime_deopt_table can find physical registers.
        for (uint32_t d = 0; d < func->ndeopt; d++) {
            XirDeoptInfo *info = &func->deopt_infos[d];
            for (uint16_t s = 0; s < info->nslots; s++) {
                XirRef ref = info->slots[s].value;
                if (!xir_ref_is_vreg(ref)) continue;
                uint32_t idx = XIR_REF_INDEX(ref);
                if (idx >= nv) continue;
                XirRef root = copy_of[idx];
                if (root != ref) {
                    info->slots[s].value = root;
                    changed = true;
                }
            }
        }

        // Call arg pool refs: CALL_C/CALL_KNOWN/etc. store argument vreg
        // references in a flat pool. phi_simp + copy_prop may replace the
        // original vreg with its copy root, but the pool was never updated,
        // causing codegen to emit stores from dead/stale registers.
        if (func->call_arg_pool) {
            for (uint32_t v = 0; v < nv; v++) {
                if (func->vregs[v].call_nargs == 0) continue;
                uint32_t start = func->vregs[v].call_arg_start;
                for (uint16_t a = 0; a < func->vregs[v].call_nargs; a++) {
                    XirRef ref = func->call_arg_pool[start + a];
                    if (!xir_ref_is_vreg(ref)) continue;
                    uint32_t idx = XIR_REF_INDEX(ref);
                    if (idx >= nv) continue;
                    XirRef root = copy_of[idx];
                    if (root != ref) {
                        func->call_arg_pool[start + a] = root;
                        changed = true;
                    }
                }
            }
        }
    }

    xr_free(copy_of);
    return iters > 1
        ? (XirPassChange){ false, false, true, 0, 0, 0 }
        : xir_pass_no_change();
}

/* ========== Phi Simplification ========== */

/*
 * Simplify phi nodes where all non-self arguments are the same value:
 *   phi(v, v, v) → MOV v   (trivial phi)
 *   phi(v, phi_dst, v) → MOV v   (self-referencing phi in loop)
 *
 * The MOV is inserted at the beginning of the block's instruction array,
 * and the phi is removed. Subsequent copy_prop + DCE will clean up.
 */
XirPassChange xir_pass_phi_simp(XirFunc *func) {
    if (!func) return xir_pass_no_change();

    uint32_t iters = 0;
    bool changed = true;
    while (changed) {
        changed = false;
        iters++;
        for (uint32_t bi = 0; bi < func->nblk; bi++) {
            XirBlock *blk = func->blocks[bi];

            XirPhi **pp = &blk->phis;
            while (*pp) {
                XirPhi *phi = *pp;

                // Find the unique non-self, non-NONE argument
                XirRef unique = XIR_NONE;
                bool trivial = true;

                for (uint32_t p = 0; p < phi->narg; p++) {
                    XirRef arg = phi->args[p];
                    if (xir_ref_is_none(arg)) continue;
                    if (arg == phi->dst) continue;
                    if (xir_ref_is_none(unique)) {
                        unique = arg;
                    } else if (arg != unique) {
                        trivial = false;
                        break;
                    }
                }

                if (trivial && !xir_ref_is_none(unique)) {
                    /* Skip if arg rep differs from phi rep — replacing
                     * would create a cross-rep MOV that breaks typed
                     * instructions after copy propagation. */
                    if (xir_ref_is_vreg(unique)) {
                        uint32_t ui = XIR_REF_INDEX(unique);
                        if (ui < func->nvreg &&
                            func->vregs[ui].rep != phi->rep) {
                            pp = &(*pp)->next;
                            continue;
                        }
                    }
                    // Replace phi with MOV: shift instructions right, insert MOV at front
                    if (blk->nins < blk->ins_cap) {
                        memmove(&blk->ins[1], &blk->ins[0],
                                blk->nins * sizeof(XirIns));
                        memset(&blk->ins[0], 0, sizeof(XirIns));
                        blk->ins[0].op = XIR_MOV;
                        blk->ins[0].rep = phi->rep;
                        blk->ins[0].dst = phi->dst;
                        blk->ins[0].args[0] = unique;
                        blk->ins[0].args[1] = XIR_NONE;
                        blk->nins++;

                        // Remove phi from list
                        *pp = phi->next;
                        changed = true;
                        continue;
                    }
                }

                pp = &(*pp)->next;
            }
        }
    }

    // PHI vtag propagation: when all incoming args have the same vtag,
    // propagate to the phi's dst vreg. This improves downstream codegen
    // and deopt precision by reducing VTAG_TAGGED occurrences.
    for (uint32_t bi = 0; bi < func->nblk; bi++) {
        XirBlock *blk = func->blocks[bi];
        for (XirPhi *phi = blk->phis; phi; phi = phi->next) {
            if (!xir_ref_is_vreg(phi->dst)) continue;
            uint32_t dvi = XIR_REF_INDEX(phi->dst);
            if (dvi >= func->nvreg) continue;

            uint8_t common_tag = VTAG_TAGGED;
            uint16_t common_ht = 0;
            bool first = true, all_same = true;

            for (uint32_t p = 0; p < phi->narg; p++) {
                XirRef arg = phi->args[p];
                if (xir_ref_is_none(arg)) continue;
                if (arg == phi->dst) continue;  // skip self-ref
                uint8_t atag = VTAG_TAGGED;
                uint16_t aht = 0;
                if (xir_ref_is_vreg(arg)) {
                    XirType act = xir_ref_ctype(func, arg);
                    atag = type_kind_to_vtag(act.kind);
                    aht = act.heap_cid;
                }
                if (first) {
                    common_tag = atag;
                    common_ht = aht;
                    first = false;
                } else if (atag != common_tag) {
                    // VTAG_BOOL covers both TRUE and FALSE; no special-case needed
                    all_same = false;
                    break;
                } else if (aht != common_ht) {
                    common_ht = 0;  // different heap_types → clear
                }
            }

            if (!first && all_same && common_tag != VTAG_TAGGED) {
                func->vregs[dvi].heap_type = common_ht;
                XirIns *def = func->vregs[dvi].def;
                if (def) {
                    def->ctype.kind = vtag_to_type_kind(common_tag);
                    def->ctype.heap_cid = common_ht;
                }
            }
        }
    }

    // PHI xrtype propagation: when all incoming args have the same
    // canonical XrType* (pointer equality), propagate to phi dst.
    for (uint32_t bi = 0; bi < func->nblk; bi++) {
        XirBlock *blk = func->blocks[bi];
        for (XirPhi *phi = blk->phis; phi; phi = phi->next) {
            if (!xir_ref_is_vreg(phi->dst)) continue;
            uint32_t dvi = XIR_REF_INDEX(phi->dst);
            if (dvi >= func->nvreg) continue;
            if (func->vregs[dvi].xrtype != NULL) continue;

            XrType *common_xrt = NULL;
            bool first_xrt = true, all_same_xrt = true;

            for (uint32_t p = 0; p < phi->narg; p++) {
                XirRef arg = phi->args[p];
                if (xir_ref_is_none(arg)) continue;
                if (arg == phi->dst) continue;
                XrType *axrt = NULL;
                if (xir_ref_is_vreg(arg)) {
                    uint32_t avi = XIR_REF_INDEX(arg);
                    if (avi < func->nvreg)
                        axrt = func->vregs[avi].xrtype;
                }
                if (!axrt) { all_same_xrt = false; break; }
                if (first_xrt) {
                    common_xrt = axrt;
                    first_xrt = false;
                } else if (axrt != common_xrt) {
                    all_same_xrt = false;
                    break;
                }
            }

            if (!first_xrt && all_same_xrt && common_xrt)
                func->vregs[dvi].xrtype = common_xrt;
        }
    }
    return iters > 1
        ? (XirPassChange){ false, true, true, 0, 0, 0 }
        : xir_pass_no_change();
}

/* ========== CFG Rebuild (QBE fillpreds model) ========== */

/*
 * Rebuild all predecessor lists from s1/s2 edges and remap phi args
 * to match the new pred ordering. This is the canonical way to restore
 * CFG consistency after any pass that modifies block structure.
 *
 * Algorithm:
 *   1. Save old pred arrays for blocks that have phis
 *   2. Clear all npred to 0
 *   3. Walk all blocks, add pred from s1/s2
 *   4. Remap phi args: for each new pred[k], find matching old pred
 *      and copy the corresponding phi arg
 */
void xir_rebuild_preds(XirFunc *func) {
    if (!func || func->nblk == 0) return;

    uint32_t nblk = func->nblk;

    // Save old pred info for blocks with phis (needed for arg remapping)
    typedef struct { XirBlock **preds; uint32_t npred; } OldPreds;
    OldPreds *old = (OldPreds *)xr_calloc(nblk, sizeof(OldPreds));
    if (!old) return;

    for (uint32_t i = 0; i < nblk; i++) {
        XirBlock *blk = func->blocks[i];
        old[i].npred = blk->npred;
        if (blk->npred > 0 && blk->phis) {
            old[i].preds = (XirBlock **)xr_malloc(blk->npred * sizeof(XirBlock *));
            if (old[i].preds)
                memcpy(old[i].preds, blk->preds, blk->npred * sizeof(XirBlock *));
        }
        blk->npred = 0;
    }

    // Rebuild from s1/s2
    for (uint32_t i = 0; i < nblk; i++) {
        XirBlock *blk = func->blocks[i];
        if (blk->s1) xir_block_add_pred(blk->s1, blk, func->arena);
        if (blk->s2 && blk->s2 != blk->s1)
            xir_block_add_pred(blk->s2, blk, func->arena);
    }

    // Remap phi args to match new pred ordering
    for (uint32_t i = 0; i < nblk; i++) {
        XirBlock *blk = func->blocks[i];
        if (!blk->phis || !old[i].preds) continue;

        for (XirPhi *phi = blk->phis; phi; phi = phi->next) {
            XirRef *new_args = (XirRef *)xir_arena_calloc(
                func->arena, blk->npred, sizeof(XirRef));
            if (!new_args) continue;

            for (uint16_t k = 0; k < blk->npred; k++) {
                new_args[k] = XIR_NONE;
                for (uint32_t j = 0; j < old[i].npred; j++) {
                    if (old[i].preds[j] == blk->preds[k] && j < phi->narg) {
                        new_args[k] = phi->args[j];
                        break;
                    }
                }
            }
            phi->args = new_args;
            phi->narg = blk->npred;
        }

        xr_free(old[i].preds);
    }

    xr_free(old);
}

/*
 * Debug-only CFG verification (run after every pass in debug builds).
 * Checks:
 *   1. Successor → pred bidirectional consistency
 *   2. Pred → successor bidirectional consistency
 *   3. phi->narg == block->npred
 *   4. Block terminator matches s1/s2 presence
 *   5. Vreg indices in range
 *   6. Phi arg vreg/const indices in range
 */
void xir_verify_cfg(XirFunc *func) {
#ifndef NDEBUG
    if (!func || func->nblk == 0) return;

    for (uint32_t i = 0; i < func->nblk; i++) {
        XirBlock *blk = func->blocks[i];

        // Check successors → preds consistency
        XirBlock *succs[2] = { blk->s1, blk->s2 };
        for (int s = 0; s < 2; s++) {
            if (!succs[s]) continue;
            bool found = false;
            for (uint32_t p = 0; p < succs[s]->npred; p++) {
                if (succs[s]->preds[p] == blk) { found = true; break; }
            }
            XR_DCHECK(found, "successor does not have block in its preds");
        }

        // Check preds → successors consistency
        for (uint32_t p = 0; p < blk->npred; p++) {
            XirBlock *pred = blk->preds[p];
            XR_DCHECK((pred->s1 == blk || pred->s2 == blk),
                   "pred does not have block as successor");
        }

        // Check phi narg == npred
        for (XirPhi *phi = blk->phis; phi; phi = phi->next) {
            XR_DCHECK(phi->narg == blk->npred,
                   "phi narg does not match block npred");
            // Check phi arg refs are valid
            for (uint16_t a = 0; a < phi->narg; a++) {
                XirRef r = phi->args[a];
                if (xir_ref_is_none(r)) continue;
                if (xir_ref_is_vreg(r))
                    XR_DCHECK(XIR_REF_INDEX(r) < func->nvreg, "phi arg vreg out of range");
                if (xir_ref_is_const(r))
                    XR_DCHECK(XIR_REF_INDEX(r) < func->nconst, "phi arg const out of range");
            }
            // Check phi dst vreg is valid
            if (xir_ref_is_vreg(phi->dst))
                XR_DCHECK(XIR_REF_INDEX(phi->dst) < func->nvreg, "phi dst vreg out of range");
        }

        // Check block terminator matches s1/s2 presence
        switch (blk->jmp.type) {
        case XIR_JMP_JMP:
            XR_DCHECK(blk->s1 != NULL, "JMP block must have s1");
            XR_DCHECK(blk->s2 == NULL, "JMP block must not have s2");
            break;
        case XIR_JMP_BR:
            XR_DCHECK(blk->s1 != NULL, "BR block must have s1");
            XR_DCHECK(blk->s2 != NULL, "BR block must have s2");
            break;
        case XIR_JMP_RET:
        case XIR_JMP_UNREACHABLE:
            XR_DCHECK(blk->s1 == NULL, "RET/UNREACHABLE block must not have s1");
            XR_DCHECK(blk->s2 == NULL, "RET/UNREACHABLE block must not have s2");
            break;
        case XIR_JMP_NONE:
            break;
        }

        // Check instruction vreg indices in range
        for (uint32_t j = 0; j < blk->nins; j++) {
            XirIns *ins = &blk->ins[j];
            if (xir_ref_is_vreg(ins->dst))
                XR_DCHECK(XIR_REF_INDEX(ins->dst) < func->nvreg, "ins dst vreg out of range");
            for (int a = 0; a < 2; a++) {
                XirRef r = ins->args[a];
                if (xir_ref_is_vreg(r))
                    XR_DCHECK(XIR_REF_INDEX(r) < func->nvreg, "ins arg vreg out of range");
                if (xir_ref_is_const(r))
                    XR_DCHECK(XIR_REF_INDEX(r) < func->nconst, "ins arg const out of range");
            }
        }

        // Check block.id consistency
        XR_DCHECK(blk->id == i, "block->id does not match index in func->blocks[]");
    }

    // Check vreg.def consistency: def pointer must point back to an instruction
    // that defines this vreg, and each vreg has at most one instruction def.
    for (uint32_t v = 0; v < func->nvreg; v++) {
        XirIns *def = func->vregs[v].def;
        if (!def) continue;  // phi-defined or unused vregs may have NULL def
        if (def->op == XIR_NOP) continue;  // NOPed by DCE, will be compacted
        XR_DCHECK(xir_ref_is_vreg(def->dst) && XIR_REF_INDEX(def->dst) == v,
                  "vreg.def does not point to instruction defining this vreg");
    }

    // Check call_arg_pool refs: every vreg referenced in the pool must be
    // a valid vreg index. This catches stale refs left by passes that
    // forgot to update the pool (e.g. copy_prop, phi_simp).
    if (func->call_arg_pool) {
        for (uint32_t v = 0; v < func->nvreg; v++) {
            if (func->vregs[v].call_nargs == 0) continue;
            uint32_t start = func->vregs[v].call_arg_start;
            XR_DCHECK(start + func->vregs[v].call_nargs <= func->call_arg_pool_used,
                      "call_arg_pool: vreg pool range out of bounds");
            for (uint16_t a = 0; a < func->vregs[v].call_nargs; a++) {
                XirRef ref = func->call_arg_pool[start + a];
                if (xir_ref_is_none(ref)) continue;
                if (xir_ref_is_vreg(ref))
                    XR_DCHECK(XIR_REF_INDEX(ref) < func->nvreg,
                              "call_arg_pool: vreg ref out of range");
                if (xir_ref_is_const(ref))
                    XR_DCHECK(XIR_REF_INDEX(ref) < func->nconst,
                              "call_arg_pool: const ref out of range");
            }
        }
    }
#else
    (void)func;
#endif
}

/*
 * Check if an XIR opcode may trigger garbage collection.
 * Used by verify_types to check ALLOC safety zones.
 */
static bool xir_op_may_gc(uint16_t op) {
    switch (op) {
    case XIR_ALLOC:
    case XIR_CALL:
    case XIR_CALL_C:
    case XIR_CALL_C_LEAF:
    case XIR_CALL_SELF_DIRECT:
    case XIR_CALL_DIRECT:
    case XIR_CALL_KNOWN:
    case XIR_CALL_KNOWN_REG:
    case XIR_SAFEPOINT:
    case XIR_RT_ARRAY_NEW:
    case XIR_RT_MAP_NEW:
    case XIR_RT_ARRAY_PUSH:
    case XIR_RT_INDEX_SET:
        return true;
    default:
        return false;
    }
}

/*
 * Debug-only type consistency verification.
 * Checks:
 *   1. Instruction dst type matches vreg type
 *   2. Phi dst type matches vreg type
 *   3. Integer arithmetic args are I64
 *   4. Float arithmetic args are F64
 *   5. BOX/UNBOX input/output types
 *   6. Type conversion (I2F/F2I) types
 *   7. Constant instruction types
 *   8. ALLOC dst is PTR
 */
void xir_verify_types(XirFunc *func) {
#ifndef NDEBUG
    if (!func || func->nblk == 0) return;

    // Helper: resolve XirRef to its XirType
    #define REF_TYPE(r) ( \
        xir_ref_is_vreg(r) ? func->vregs[XIR_REF_INDEX(r)].rep : \
        xir_ref_is_const(r) ? func->consts[XIR_REF_INDEX(r)].rep : \
        XR_REP_VOID)

    for (uint32_t i = 0; i < func->nblk; i++) {
        XirBlock *blk = func->blocks[i];

        // Check phi type consistency
        for (XirPhi *phi = blk->phis; phi; phi = phi->next) {
            if (xir_ref_is_vreg(phi->dst)) {
                uint32_t vi = XIR_REF_INDEX(phi->dst);
                XR_DCHECK(phi->rep == func->vregs[vi].rep,
                       "phi type does not match vreg type");
            }
        }

        for (uint32_t j = 0; j < blk->nins; j++) {
            XirIns *ins = &blk->ins[j];

            // Check 1: dst vreg type matches instruction type
            if (xir_ref_is_vreg(ins->dst)) {
                uint32_t vi = XIR_REF_INDEX(ins->dst);
                XR_DCHECK(ins->rep == func->vregs[vi].rep,
                       "ins type does not match dst vreg type");
            }

            // Check 2: float arithmetic args must be F64
            if (ins->op >= XIR_FADD && ins->op <= XIR_FNEG) {
                for (int a = 0; a < 2; a++) {
                    if (xir_ref_is_none(ins->args[a])) continue;
                    uint8_t at = REF_TYPE(ins->args[a]);
                    XR_DCHECK(at == XR_REP_F64,
                           "float op arg is not F64");
                }
            }

            // Check 4: integer comparison args must be I64, PTR, or TAGGED
            if (ins->op >= XIR_EQ && ins->op <= XIR_GE) {
                for (int a = 0; a < 2; a++) {
                    if (xir_ref_is_none(ins->args[a])) continue;
                    uint8_t at = REF_TYPE(ins->args[a]);
                    XR_DCHECK(at == XR_REP_I64 || at == XR_REP_PTR ||
                             at == XR_REP_TAGGED,
                             "int comparison arg has unexpected rep");
                }
            }

            // Check 5: float comparison args must be F64
            if (ins->op >= XIR_FEQ && ins->op <= XIR_FLE) {
                for (int a = 0; a < 2; a++) {
                    if (xir_ref_is_none(ins->args[a])) continue;
                    uint8_t at = REF_TYPE(ins->args[a]);
                    XR_DCHECK(at == XR_REP_F64,
                           "float comparison arg is not F64");
                }
            }

            // Check 6: BOX/UNBOX type constraints
            switch (ins->op) {
            case XIR_BOX_I64:
                XR_DCHECK(ins->rep == XR_REP_TAGGED, "BOX_I64 dst not TAGGED");
                if (!xir_ref_is_none(ins->args[0]))
                    XR_DCHECK(REF_TYPE(ins->args[0]) == XR_REP_I64, "BOX_I64 arg not I64");
                break;
            case XIR_BOX_F64:
                XR_DCHECK(ins->rep == XR_REP_TAGGED, "BOX_F64 dst not TAGGED");
                if (!xir_ref_is_none(ins->args[0]))
                    XR_DCHECK(REF_TYPE(ins->args[0]) == XR_REP_F64, "BOX_F64 arg not F64");
                break;
            case XIR_UNBOX_I64:
                XR_DCHECK(ins->rep == XR_REP_I64, "UNBOX_I64 dst not I64");
                if (!xir_ref_is_none(ins->args[0]))
                    XR_DCHECK(REF_TYPE(ins->args[0]) == XR_REP_TAGGED, "UNBOX_I64 arg not TAGGED");
                break;
            case XIR_UNBOX_F64:
                XR_DCHECK(ins->rep == XR_REP_F64, "UNBOX_F64 dst not F64");
                if (!xir_ref_is_none(ins->args[0]))
                    XR_DCHECK(REF_TYPE(ins->args[0]) == XR_REP_TAGGED, "UNBOX_F64 arg not TAGGED");
                break;

            // Check 7: type conversion
            case XIR_I2F:
                XR_DCHECK(ins->rep == XR_REP_F64, "I2F dst not F64");
                if (!xir_ref_is_none(ins->args[0]))
                    XR_DCHECK(REF_TYPE(ins->args[0]) == XR_REP_I64, "I2F arg not I64");
                break;
            case XIR_F2I:
                XR_DCHECK(ins->rep == XR_REP_I64, "F2I dst not I64");
                if (!xir_ref_is_none(ins->args[0]))
                    XR_DCHECK(REF_TYPE(ins->args[0]) == XR_REP_F64, "F2I arg not F64");
                break;

            // Check 8: constant types
            case XIR_CONST_I64:
                XR_DCHECK(ins->rep == XR_REP_I64, "CONST_I64 type not I64");
                break;
            case XIR_CONST_F64:
                XR_DCHECK(ins->rep == XR_REP_F64, "CONST_F64 type not F64");
                break;
            case XIR_CONST_PTR:
                XR_DCHECK(ins->rep == XR_REP_PTR, "CONST_PTR type not PTR");
                break;

            // Check 9: ALLOC dst must be PTR
            case XIR_ALLOC:
                XR_DCHECK(ins->rep == XR_REP_PTR, "ALLOC dst not PTR");
                break;

            default:
                break;
            }
        }
    }

    // Check vtag/rep consistency for all vregs:
    // Concrete vtag must agree with machine rep. For example,
    // VTAG_I64 requires rep==I64; VTAG_F64 requires rep==F64.
    // This catches stale vtag left by passes that forgot to
    // update vtag when changing rep (or vice versa).
    for (uint32_t v = 0; v < func->nvreg; v++) {
        uint8_t vt = type_kind_to_vtag(xir_ref_ctype(func, XIR_REF(XIR_REF_VREG, v)).kind);
        uint8_t rp = func->vregs[v].rep;
        switch (vt) {
        case VTAG_F64:
            // F64 vtag must use FP or TAGGED register
            XR_DCHECK(rp == XR_REP_F64 || rp == XR_REP_TAGGED,
                      "VTAG_F64 but rep is not F64 or TAGGED");
            break;
        // Note: VTAG_I64/PTR/BOOL with mismatched rep can occur when
        // GUARD_TAG narrows vtag after select_rep chose a different rep.
        // Codegen and regalloc handle this correctly (GP regs for I64/PTR/BOOL,
        // FP regs for F64). No assertion needed for these combinations.
        default:
            break;
        }
    }

    /* ---- ALLOC safety zone check ----
     * Verify that between an ALLOC and the next may-GC instruction in
     * the same block, the ALLOC result is used by at least one
     * STORE_FIELD (object partially initialized before GC can fire).
     * This prevents scanning uninitialized GC pointer fields.
     */
    for (uint32_t i = 0; i < func->nblk; i++) {
        XirBlock *blk = func->blocks[i];
        for (uint32_t j = 0; j < blk->nins; j++) {
            XirIns *ins = &blk->ins[j];
            if (ins->op != XIR_ALLOC) continue;
            XirRef alloc_dst = ins->dst;
            bool initialized = false;
            for (uint32_t k = j + 1; k < blk->nins; k++) {
                XirIns *next = &blk->ins[k];
                // STORE_FIELD using alloc result as object ptr
                if (next->op == XIR_STORE_FIELD && next->args[0] == alloc_dst) {
                    initialized = true;
                    break;
                }
                // Another may-GC instruction before any STORE_FIELD init
                if (xir_op_may_gc(next->op)) {
                    XR_DCHECK(initialized,
                           "ALLOC result not initialized before next may-GC instruction");
                    break;
                }
            }
        }
    }

    #undef REF_TYPE
#else
    (void)func;
#endif
}

/* ========== Critical Edge Splitting ========== */

/*
 * Split critical edges: insert empty blocks on edges from a block with
 * multiple successors (BR) to a block with multiple predecessors.
 * This enables clean phi resolution and reduces unnecessary register copies.
 */
XirPassChange xir_pass_split_critical_edges(XirFunc *func) {
    if (!func || func->nblk == 0) return xir_pass_no_change();

    // Collect critical edges first to avoid mutation during iteration
    typedef struct { uint32_t from_bi; int succ_slot; } CritEdge;
    CritEdge edges[128];
    uint32_t nedges = 0;

    for (uint32_t bi = 0; bi < func->nblk && nedges < 128; bi++) {
        XirBlock *blk = func->blocks[bi];
        if (blk->jmp.type != XIR_JMP_BR) continue;
        // BR has two successors — check each
        XirBlock *succs[2] = { blk->s1, blk->s2 };
        for (int s = 0; s < 2; s++) {
            if (succs[s] && succs[s]->npred > 1 && nedges < 128) {
                edges[nedges++] = (CritEdge){ bi, s };
            }
        }
    }

    for (uint32_t ei = 0; ei < nedges; ei++) {
        XirBlock *from = func->blocks[edges[ei].from_bi];
        int slot = edges[ei].succ_slot;
        XirBlock *target = (slot == 0) ? from->s1 : from->s2;
        if (!target) continue;

        // Insert empty block: from → mid → target
        XirBlock *mid = xir_func_add_block(func, NULL);
        if (!mid) continue;
        mid->jmp.type = XIR_JMP_JMP;
        mid->jmp.arg = XIR_NONE;
        mid->s1 = target;

        // Update from's successor pointer
        if (slot == 0) from->s1 = mid;
        else           from->s2 = mid;

        // Add from as mid's predecessor
        xir_block_add_pred(mid, from, func->arena);

        // Update target's predecessor list: replace from with mid
        for (uint32_t p = 0; p < target->npred; p++) {
            if (target->preds[p] == from) {
                target->preds[p] = mid;
                break;
            }
        }

        /* Update phi arguments in target: args corresponding to 'from'
         * now correspond to 'mid' (pred index unchanged, just the block ptr) */
        /* No phi arg values change — only the predecessor identity changes,
         * which is already handled by updating preds[p] above. */
    }
    return nedges > 0
        ? (XirPassChange){ true, false, false, 0, 0, 0 }
        : xir_pass_no_change();
}

/*
 * Merge a block A with its sole successor B when:
 *   1. A has exactly one successor (JMP, not BR/RET)
 *   2. B has exactly one predecessor (A)
 *   3. B has no phi nodes (single pred means phis are trivial anyway)
 *
 * The merge appends B's instructions to A, copies B's terminator/successors,
 * and removes B from the block array.
 *
 * Iterates until no more merges are possible.
 */
XirPassChange xir_pass_merge_blocks(XirFunc *func) {
    if (!func || func->nblk <= 1) return xir_pass_no_change();

    uint32_t iters = 0;
    bool changed = true;
    while (changed) {
        changed = false;
        iters++;

        for (uint32_t bi = 0; bi < func->nblk; bi++) {
            XirBlock *a = func->blocks[bi];
            if (a->jmp.type != XIR_JMP_JMP) continue;

            XirBlock *b = a->s1;
            if (!b) continue;
            if (b->npred != 1) continue;
            if (b->phis != NULL) continue;

            // Do not merge if B is an exception handler target —
            // other blocks' exception_handler pointers would go stale.
            bool is_exc_target = false;
            for (uint32_t j = 0; j < func->nblk && !is_exc_target; j++) {
                if (func->blocks[j]->exception_handler == b)
                    is_exc_target = true;
            }
            if (is_exc_target) continue;
            if (b == a) continue;

            // Merge B's instructions into A
            uint32_t total = a->nins + b->nins;
            if (total > a->ins_cap) {
                // Not enough capacity — skip this merge
                continue;
            }
            memcpy(&a->ins[a->nins], b->ins, b->nins * sizeof(XirIns));
            a->nins = total;

            // Copy B's terminator and successors
            a->jmp = b->jmp;
            a->s1 = b->s1;
            a->s2 = b->s2;

            // Update predecessor lists of B's successors: replace B with A
            XirBlock *succs[2] = { b->s1, b->s2 };
            for (int s = 0; s < 2; s++) {
                if (!succs[s]) continue;
                for (uint32_t p = 0; p < succs[s]->npred; p++) {
                    if (succs[s]->preds[p] == b)
                        succs[s]->preds[p] = a;
                }
            }

            // Remove B from block array
            uint32_t b_idx = UINT32_MAX;
            for (uint32_t j = 0; j < func->nblk; j++) {
                if (func->blocks[j] == b) { b_idx = j; break; }
            }
            if (b_idx < func->nblk) {
                for (uint32_t j = b_idx; j + 1 < func->nblk; j++) {
                    func->blocks[j] = func->blocks[j + 1];
                    func->blocks[j]->id = j;
                }
                func->nblk--;
                changed = true;
                break;
            }
        }
    }
    return iters > 1
        ? (XirPassChange){ true, false, false, 0, 0, 0 }
        : xir_pass_no_change();
}

/* ========== SelectRepresentations ========== */

/*
 * Eliminate redundant BOX/UNBOX pairs:
 *   UNBOX_I64(BOX_I64(x)) → MOV x  (identity)
 *   UNBOX_F64(BOX_F64(x)) → MOV x
 *   BOX_I64(UNBOX_I64(x)) → MOV x
 *   BOX_F64(UNBOX_F64(x)) → MOV x
 *
 * Also collapses chains: if a BOX/UNBOX feeds directly into its
 * inverse, the intermediate is eliminated. The MOV will later be
 * cleaned up by DCE if its result is unused, or by regalloc if
 * src == dst.
 */
XirPassChange xir_pass_select_rep(XirFunc *func) {
    if (!func || func->nvreg == 0) return xir_pass_no_change();

    uint32_t nv = func->nvreg;

    // Build def map: vreg → instruction pointer
    XirIns **def_ins = (XirIns **)xr_calloc(nv, sizeof(XirIns *));
    if (!def_ins) return xir_pass_no_change();

    for (uint32_t bi = 0; bi < func->nblk; bi++) {
        XirBlock *blk = func->blocks[bi];
        for (uint32_t i = 0; i < blk->nins; i++) {
            XirIns *ins = &blk->ins[i];
            if (xir_ref_is_vreg(ins->dst)) {
                uint32_t idx = XIR_REF_INDEX(ins->dst);
                if (idx < nv) def_ins[idx] = ins;
            }
        }
    }

    // Scan for BOX/UNBOX pairs
    uint32_t iters = 0;
    bool changed = true;
    while (changed) {
        changed = false;
        iters++;
        for (uint32_t bi = 0; bi < func->nblk; bi++) {
            XirBlock *blk = func->blocks[bi];
            for (uint32_t i = 0; i < blk->nins; i++) {
                XirIns *ins = &blk->ins[i];
                uint16_t op = ins->op;

                // Check if this is an UNBOX or BOX with a single vreg arg
                uint16_t inverse_op = 0;
                if (op == XIR_UNBOX_I64)      inverse_op = XIR_BOX_I64;
                else if (op == XIR_UNBOX_F64) inverse_op = XIR_BOX_F64;
                else if (op == XIR_BOX_I64)   inverse_op = XIR_UNBOX_I64;
                else if (op == XIR_BOX_F64)   inverse_op = XIR_UNBOX_F64;
                else continue;

                if (!xir_ref_is_vreg(ins->args[0])) continue;
                uint32_t src_idx = XIR_REF_INDEX(ins->args[0]);
                if (src_idx >= nv || !def_ins[src_idx]) continue;

                XirIns *src_ins = def_ins[src_idx];
                if (src_ins->op == inverse_op) {
                    // Found a pair: replace with MOV from the original value
                    ins->op = XIR_MOV;
                    ins->args[0] = src_ins->args[0];
                    ins->args[1] = XIR_NONE;
                    changed = true;
                }
            }
        }
    }

    xr_free(def_ins);
    return iters > 1
        ? (XirPassChange){ false, false, true, 0, 0, 0 }
        : xir_pass_no_change();
}

/* ========== Block Reordering (Profile-Guided) ========== */

/*
 * Heuristic edge weight for block selection.
 *
 * Priority (higher wins):
 *   - Non-deferred > deferred (cold paths sink to end)
 *   - Higher loop depth > lower (keep loop bodies compact)
 *   - Loop header preference when continuing a loop body
 *
 * This replaces pure fall-through chaining with a profile-guided
 * strategy that uses structural hints (is_deferred, loop_depth)
 * as a proxy for edge frequency.
 */
static int32_t blk_weight(XirFunc *func, XirBlock *blk) {
    if (!blk) return -1;
    int32_t w = (int32_t)xir_block_loop_depth(func, blk->id) * 2;
    if (blk->is_deferred) w -= 1000; // push cold blocks to end
    if (blk->is_loop_header) w += 1; // slight preference to keep header adjacent
    return w;
}

/*
 * Reorder func->blocks[] to minimize taken branches.
 *
 * Strategy (profile-guided greedy fall-through chaining):
 *   1. Start from entry block, greedily follow fall-through edges.
 *   2. For BR blocks: pick the higher-weight successor as fall-through.
 *      Deferred (catch/deopt) successors are deprioritized.
 *   3. For JMP blocks: s1 is the only successor → prefer it as next.
 *   4. When both successors are placed, pick the unplaced block with
 *      highest weight (non-deferred first, then highest loop depth).
 *
 * Edge weight heuristics replace exact counters, matching the approach
 * used by V8 TurboFan and Dart when branch profile data is unavailable.
 */
XirPassChange xir_pass_reorder_blocks(XirFunc *func) {
    if (!func || func->nblk <= 2) return xir_pass_no_change();

    uint32_t n = func->nblk;
    XirBlock **order = (XirBlock **)xr_malloc(n * sizeof(XirBlock *));
    if (!order) return xir_pass_no_change();

    bool *placed = (bool *)xr_calloc(n, sizeof(bool));
    if (!placed) { xr_free(order); return xir_pass_no_change(); }

    // Build block-index lookup: block->id → index in func->blocks[]
    uint32_t max_id = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (func->blocks[i]->id > max_id)
            max_id = func->blocks[i]->id;
    }
    int32_t *id_to_idx = (int32_t *)xr_malloc((max_id + 1) * sizeof(int32_t));
    if (!id_to_idx) { xr_free(placed); xr_free(order); return xir_pass_no_change(); }
    for (uint32_t i = 0; i <= max_id; i++) id_to_idx[i] = -1;
    for (uint32_t i = 0; i < n; i++) id_to_idx[func->blocks[i]->id] = (int32_t)i;

    uint32_t norder = 0;

    #define BLK_IDX(blk) ((blk) && (blk)->id <= max_id ? id_to_idx[(blk)->id] : -1)

    // Place entry block first
    order[norder++] = func->blocks[0];
    placed[0] = true;

    // Greedy walk: follow best-weight fall-through edges
    XirBlock *cur = func->blocks[0];
    while (norder < n) {
        XirBlock *next = NULL;

        if (cur) {
            XirBlock *cand1 = NULL, *cand2 = NULL;

            if (cur->jmp.type == XIR_JMP_BR) {
                // Profile-guided: if branch_taken_pct is set, prefer the
                // more-likely successor as the fall-through candidate.
                //   s1 = taken (true) path, s2 = not-taken (false) path.
                //   branch_taken_pct > 50 → s1 is hot → fall through to s1.
                //   branch_taken_pct <= 50 (or 0=unknown) → use heuristic.
                if (cur->branch_taken_pct > 50) {
                    cand1 = cur->s1; // hot taken path as fall-through
                    cand2 = cur->s2;
                } else if (cur->branch_taken_pct > 0) {
                    cand1 = cur->s2; // hot not-taken path as fall-through
                    cand2 = cur->s1;
                } else if (cur->is_loop_header && cur->s1 &&
                           !cur->s1->is_deferred) {
                    cand1 = cur->s1; // loop body as fall-through
                    cand2 = cur->s2;
                } else {
                    cand1 = cur->s2; // default: false path as fall-through
                    cand2 = cur->s1;
                }
            } else if (cur->jmp.type == XIR_JMP_JMP) {
                cand1 = cur->s1;
            }

            // Try primary candidate
            if (cand1) {
                int32_t idx = BLK_IDX(cand1);
                if (idx >= 0 && !placed[idx]) {
                    next = cand1;
                    placed[idx] = true;
                }
            }
            // Try secondary candidate
            if (!next && cand2) {
                int32_t idx = BLK_IDX(cand2);
                if (idx >= 0 && !placed[idx]) {
                    next = cand2;
                    placed[idx] = true;
                }
            }
        }

        // No successor available — pick unplaced block with highest weight.
        // Non-deferred blocks with high loop depth come first; deferred
        // (catch/deopt) blocks naturally sink to the end of the function.
        if (!next) {
            int32_t best_w = INT32_MIN;
            uint32_t best_idx = 0;
            bool found = false;
            for (uint32_t i = 0; i < n; i++) {
                if (placed[i]) continue;
                int32_t w = blk_weight(func, func->blocks[i]);
                if (!found || w > best_w) {
                    best_w = w;
                    best_idx = i;
                    found = true;
                }
            }
            XR_DCHECK(found, "reorder: no unplaced block but norder < n");
            next = func->blocks[best_idx];
            placed[best_idx] = true;
        }

        order[norder++] = next;
        cur = next;
    }

    #undef BLK_IDX

    // Apply new order and update block ids
    for (uint32_t i = 0; i < n; i++) {
        func->blocks[i] = order[i];
        func->blocks[i]->id = i;
    }

    xr_free(id_to_idx);
    xr_free(placed);
    xr_free(order);
    return (XirPassChange){ true, false, false, 0, 0, 0 };
}

