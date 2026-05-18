/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xm_pass_advanced.c - Xm advanced optimization passes
 *
 * KEY CONCEPT:
 *   Function inlining, write barriers, ARC releases, escape analysis,
 *   scalar replacement, auto-inlining, redundant guard elimination,
 *   branch value propagation, global code motion, and type propagation.
 */

#include "xm_pass_internal.h"
#include "xm_pass_limits.h"
#include "xm_domtree.h"
#include "xm_looptree.h"
#include "xi_to_xm.h"
#include "../base/xchecks.h"

/* ========== Function Inlining ========== */

static void inline_replace_pred(XmBlock *succ, XmBlock *old_pred, XmBlock *new_pred) {
    if (!succ || !old_pred || !new_pred)
        return;
    for (uint32_t i = 0; i < succ->npred; i++) {
        if (succ->preds[i] == old_pred) {
            succ->preds[i] = new_pred;
            return;
        }
    }
}

static void inline_emit_copy(XmFunc *func, XmBlock *blk, const XmIns *src) {
    xm_emit_raw(func, blk, src->op, src->rep, src->dst, src->args[0], src->args[1]);
    XmIns *dst = &blk->ins[blk->nins - 1];
    dst->flags = src->flags;
    dst->ctype = src->ctype;
    if (xm_ref_is_vreg(dst->dst)) {
        uint32_t vi = XM_REF_INDEX(dst->dst);
        if (vi < func->nvreg)
            func->vregs[vi].def = dst;
    }
}

/*
 * Inline callee's IR into caller at a specific call site.
 *
 * Algorithm:
 *   1. Split call_block at call_ins_idx into [pre_call | post_call]
 *   2. Clone callee's blocks into caller with vreg/const/block remapping
 *   3. Map callee params to call_args
 *   4. Replace callee RET terminators with JMP to post_call
 *   5. Wire pre_call → callee entry, post_call receives return value via Phi
 */
XmRef xm_inline_function(XmFunc *caller, XmBlock *call_block, uint32_t call_ins_idx, XmFunc *callee,
                         XmRef *call_args, uint32_t nargs) {
    if (!caller || !callee || !call_block)
        return XM_NONE;
    if (callee->nblk == 0)
        return XM_NONE;

    // --- Step 1: Remap tables ---
    // Vreg remap: callee vreg index → new caller vreg ref
    uint32_t callee_nvreg = callee->nvreg;
    XmRef *vreg_map = (XmRef *) xr_malloc(callee_nvreg * sizeof(XmRef));
    if (!vreg_map)
        return XM_NONE;
    for (uint32_t i = 0; i < callee_nvreg; i++)
        vreg_map[i] = XM_NONE;

    // Map callee params (vreg 0..nargs-1) to call arguments
    uint32_t map_count = (nargs < callee_nvreg) ? nargs : callee_nvreg;
    for (uint32_t i = 0; i < map_count; i++) {
        vreg_map[i] = call_args[i];
    }

    // Allocate new vregs for non-param callee vregs (propagate metadata)
    for (uint32_t i = nargs; i < callee_nvreg; i++) {
        vreg_map[i] = xm_new_vreg(caller, callee->vregs[i].rep);
        if (xm_ref_is_vreg(vreg_map[i])) {
            uint32_t vi = XM_REF_INDEX(vreg_map[i]);
            if (vi < caller->nvreg) {
                caller->vregs[vi].heap_type = callee->vregs[i].heap_type;
                caller->vregs[vi].xrtype = callee->vregs[i].xrtype;
                caller->vregs[vi].callee_proto = callee->vregs[i].callee_proto;
                caller->vregs[vi].layout = callee->vregs[i].layout;
                caller->vregs[vi].struct_idx = callee->vregs[i].struct_idx;
            }
        }
    }

    // Const remap: callee const index → new caller const ref
    uint32_t nconst = callee->nconst;
    XmRef *const_map = (XmRef *) xr_malloc(nconst * sizeof(XmRef));
    if (!const_map) {
        xr_free(vreg_map);
        return XM_NONE;
    }
    for (uint32_t i = 0; i < nconst; i++) {
        XmConst *c = &callee->consts[i];
        if (c->rep == XR_REP_F64) {
            const_map[i] = xm_const_f64(caller, c->val.f64);
        } else {
            const_map[i] = xm_const_i64(caller, c->val.i64);
        }
    }

// Helper: remap a single XmRef from callee space to caller space
#define REMAP_REF(r)                                                                               \
    do {                                                                                           \
        if (xm_ref_is_vreg(r)) {                                                                   \
            uint32_t _idx = XM_REF_INDEX(r);                                                       \
            if (_idx < callee_nvreg)                                                               \
                (r) = vreg_map[_idx];                                                              \
        } else if (xm_ref_is_const(r)) {                                                           \
            uint32_t _idx = XM_REF_INDEX(r);                                                       \
            if (_idx < nconst)                                                                     \
                (r) = const_map[_idx];                                                             \
        }                                                                                          \
    } while (0)

    // --- Step 2: Split call_block ---
    // Create continuation block (instructions after the call)
    XmBlock *cont_blk = xm_func_add_block(caller, "inline.cont");

    // Move instructions after call_ins_idx to cont_blk
    uint32_t post_start = call_ins_idx + 1;
    for (uint32_t i = post_start; i < call_block->nins; i++) {
        XmIns *src = &call_block->ins[i];
        inline_emit_copy(caller, cont_blk, src);
    }

    // Copy terminator to cont_blk
    cont_blk->jmp = call_block->jmp;
    cont_blk->s1 = call_block->s1;
    cont_blk->s2 = call_block->s2;
    inline_replace_pred(cont_blk->s1, call_block, cont_blk);
    if (cont_blk->s2 != cont_blk->s1)
        inline_replace_pred(cont_blk->s2, call_block, cont_blk);

    // Truncate call_block (remove the call instruction and everything after)
    call_block->nins = call_ins_idx;

    // --- Step 3: Clone callee blocks ---
    XmBlock **cloned_blocks = (XmBlock **) xr_malloc(callee->nblk * sizeof(XmBlock *));
    if (!cloned_blocks) {
        xr_free(vreg_map);
        xr_free(const_map);
        return XM_NONE;
    }

    for (uint32_t bi = 0; bi < callee->nblk; bi++) {
        char label[64];
        snprintf(label, sizeof(label), "inline.%s.%u",
                 callee->blocks[bi]->label ? callee->blocks[bi]->label : "b", bi);
        cloned_blocks[bi] = xm_func_add_block(caller, label);
    }

    // Collect return blocks and their return values for Phi
    XmBlock *ret_blocks[32];
    XmRef ret_values[32];
    uint32_t nret = 0;

    for (uint32_t bi = 0; bi < callee->nblk; bi++) {
        XmBlock *src_blk = callee->blocks[bi];
        XmBlock *dst_blk = cloned_blocks[bi];

        // Clone instructions with remapping
        for (uint32_t i = 0; i < src_blk->nins; i++) {
            XmIns ins = src_blk->ins[i];  // copy
            uint32_t src_dst_idx = xm_ref_is_vreg(ins.dst) ? XM_REF_INDEX(ins.dst) : UINT32_MAX;
            // Remap dst and args
            if (xm_ref_is_vreg(ins.dst)) {
                uint32_t idx = XM_REF_INDEX(ins.dst);
                if (idx < callee_nvreg)
                    ins.dst = vreg_map[idx];
            }
            REMAP_REF(ins.args[0]);
            REMAP_REF(ins.args[1]);

            inline_emit_copy(caller, dst_blk, &ins);

            if (src_dst_idx < callee_nvreg && xm_ref_is_vreg(ins.dst)) {
                XmVReg *src_vr = &callee->vregs[src_dst_idx];
                if (src_vr->call_nargs > 0 && callee->call_arg_pool &&
                    src_vr->call_arg_start + src_vr->call_nargs <= callee->call_arg_pool_used &&
                    src_vr->call_nargs <= 16) {
                    XmRef remapped_args[16];
                    for (uint16_t ai = 0; ai < src_vr->call_nargs; ai++) {
                        remapped_args[ai] = callee->call_arg_pool[src_vr->call_arg_start + ai];
                        REMAP_REF(remapped_args[ai]);
                    }
                    xm_func_bind_call_args(caller, ins.dst, remapped_args, src_vr->call_nargs);
                }
            }
        }

        // Clone terminator
        switch (src_blk->jmp.type) {
            case XM_JMP_JMP: {
                uint32_t target_id = src_blk->s1->id;
                if (target_id >= callee->nblk)
                    break;
                xm_block_set_jmp(dst_blk, cloned_blocks[target_id]);
                xm_block_add_pred(cloned_blocks[target_id], dst_blk, caller->arena);
                break;
            }
            case XM_JMP_BR: {
                XmRef cond = src_blk->jmp.arg;
                REMAP_REF(cond);
                uint32_t s1_id = src_blk->s1->id;
                uint32_t s2_id = src_blk->s2->id;
                if (s1_id >= callee->nblk || s2_id >= callee->nblk)
                    break;
                xm_block_set_br(dst_blk, cond, cloned_blocks[s1_id], cloned_blocks[s2_id]);
                xm_block_add_pred(cloned_blocks[s1_id], dst_blk, caller->arena);
                xm_block_add_pred(cloned_blocks[s2_id], dst_blk, caller->arena);
                break;
            }
            case XM_JMP_RET: {
                // Replace RET with JMP to continuation
                XmRef ret_val = src_blk->jmp.arg;
                REMAP_REF(ret_val);
                xm_block_set_jmp(dst_blk, cont_blk);
                xm_block_add_pred(cont_blk, dst_blk, caller->arena);
                if (nret < 32) {
                    ret_blocks[nret] = dst_blk;
                    ret_values[nret] = ret_val;
                    nret++;
                }
                break;
            }
            default:
                break;
        }

        // Clone Phi nodes
        for (XmPhi *phi = src_blk->phis; phi; phi = phi->next) {
            XmPhi *new_phi = xm_add_phi(caller, dst_blk, phi->rep);
            // Remap phi dst
            uint32_t phi_dst_idx = XM_REF_INDEX(phi->dst);
            if (phi_dst_idx < callee_nvreg) {
                new_phi->dst = vreg_map[phi_dst_idx];
            }
            // Remap phi args (need to match remapped preds)
            for (uint32_t p = 0; p < phi->narg; p++) {
                XmRef arg = phi->args[p];
                REMAP_REF(arg);
                xm_phi_set_arg(new_phi, p, arg);
            }
        }
    }

#undef REMAP_REF

    // --- Step 4: Wire call_block → callee entry ---
    xm_block_set_jmp(call_block, cloned_blocks[0]);
    xm_block_add_pred(cloned_blocks[0], call_block, caller->arena);

    // --- Step 5: Create return value Phi in cont_blk ---
    XmRef result_ref = XM_NONE;
    if (nret == 1) {
        // Single return: no Phi needed, just use the value directly
        result_ref = ret_values[0];
    } else if (nret > 1) {
        // Multiple return paths: create Phi in cont_blk
        uint8_t ret_type = (callee->proto && callee->proto->return_type_info)
                               ? xr_type_rep(callee->proto->return_type_info)
                               : XR_REP_TAGGED;
        if (ret_type == XR_REP_TAGGED)
            ret_type = XR_REP_I64;
        XmPhi *ret_phi = xm_add_phi(caller, cont_blk, ret_type);
        for (uint32_t i = 0; i < nret; i++) {
            // Find pred index for this return block
            for (uint32_t p = 0; p < cont_blk->npred; p++) {
                if (cont_blk->preds[p] == ret_blocks[i]) {
                    xm_phi_set_arg(ret_phi, p, ret_values[i]);
                    break;
                }
            }
        }
        result_ref = ret_phi->dst;
    }

    xr_free(vreg_map);
    xr_free(const_map);
    xr_free(cloned_blocks);
    return result_ref;
}

/* ========== InsertWriteBarriers ========== */

/*
 * Scan Xm for pointer store operations and insert write barrier instructions.
 * - XM_STORE_FIELD → insert XM_BARRIER_FWD (forward barrier: mark child)
 *
 * This pass rebuilds each block's instruction array with barriers interleaved.
 */
/*
 * Check if a STORE_FIELD needs a write barrier.
 * Uses ins->rep (xr_tag) first; for RUNTIME (0xFF), consults ctype.
 */
static bool sf_needs_barrier(XmFunc *func, XmIns *ins) {
    // Freshly allocated containers are guaranteed WHITE — no barrier needed
    if (ins->flags & XM_FLAG_NO_BARRIER)
        return false;

    uint8_t tag = ins->rep;
    if (tag == XR_TAG_PTR)
        return true;
    if (tag != 0xFF /* XM_SF_TAG_RUNTIME */)
        return false;

    // RUNTIME: consult value ctype for a more precise decision
    XmRef val = ins->args[1];
    if (xm_ref_is_vreg(val)) {
        uint32_t vi = XM_REF_INDEX(val);
        if (vi < func->nvreg) {
            XmType vct = xm_ref_ctype(func, val);
            uint8_t vk = type_kind_to_vtag(vct.kind);
            if (vk != VTAG_TAGGED) {
                return (vk == VTAG_PTR);
            }
            // UNKNOWN tag but machine type may help
            uint8_t mt = func->vregs[vi].rep;
            return (mt == XR_REP_PTR || mt == XR_REP_TAGGED);
        }
    }
    return true;  // conservative
}

XmPassChange xm_insert_write_barriers(XmFunc *func) {
    if (!func || !func->arena)
        return xm_pass_no_change();

    uint32_t total_barriers = 0;

    for (uint32_t bi = 0; bi < func->nblk; bi++) {
        XmBlock *blk = func->blocks[bi];

        uint32_t barrier_count = 0;
        for (uint32_t i = 0; i < blk->nins; i++) {
            if (blk->ins[i].op == XM_STORE_FIELD && sf_needs_barrier(func, &blk->ins[i]))
                barrier_count++;
        }
        if (barrier_count == 0)
            continue;

        uint32_t new_nins = blk->nins + barrier_count;
        XmIns *new_ins = (XmIns *) xm_arena_alloc(func->arena, new_nins * sizeof(XmIns));
        if (!new_ins)
            continue;

        uint32_t j = 0;
        for (uint32_t i = 0; i < blk->nins; i++) {
            new_ins[j++] = blk->ins[i];

            if (blk->ins[i].op == XM_STORE_FIELD && sf_needs_barrier(func, &blk->ins[i])) {
                XmIns barrier;
                memset(&barrier, 0, sizeof(barrier));
                barrier.op = XM_BARRIER_FWD;
                barrier.rep = XR_REP_VOID;
                barrier.flags = XM_FLAG_SIDE_EFFECT;
                barrier.dst = XM_NONE;
                barrier.args[0] = blk->ins[i].args[0];  // parent
                barrier.args[1] = blk->ins[i].args[1];  // child/value
                new_ins[j++] = barrier;
            }
        }

        XR_DCHECK(j == new_nins, "instruction count mismatch after rewrite");
        blk->ins = new_ins;
        blk->nins = new_nins;
        total_barriers += barrier_count;
    }
    return total_barriers ? (XmPassChange) {false, true, false, 0, 0, 0} : xm_pass_no_change();
}

/* ========== InsertArcReleases (AOT only) ========== */

/*
 * Per-block, per-vreg last-use release insertion.
 *
 * For each basic block, for each PTR/TAGGED vreg allocated in that block
 * (by ALLOC, RT_ARRAY_NEW, RT_MAP_NEW, or any CALL returning PTR):
 *
 *   1. Find the last instruction in the block that uses the vreg.
 *   2. Insert XM_RELEASE immediately after that instruction.
 *
 * Safety guards:
 *   - Skip if the vreg appears in a successor phi (escapes the block).
 *   - Skip if the vreg is the block's RET value (ownership to caller).
 *   - Skip if the vreg is stored into a struct field via STORE_FIELD
 *     (ownership transferred to the container, deinit handles it).
 *   - Skip function parameters (owned by caller).
 *
 * This handles both:
 *   - Single-block temporaries (loop-body makeTree results)
 *   - Non-escaping temporaries in entry blocks (stretch tree, long-lived tree)
 */
static bool xm_ref_uses_vreg(XmRef a0, XmRef a1, XmRef r) {
    return (xm_ref_is_vreg(a0) && a0 == r) || (xm_ref_is_vreg(a1) && a1 == r);
}

XmPassChange xm_insert_arc_releases(XmFunc *func) {
    if (!func || !func->arena)
        return xm_pass_no_change();

    uint32_t nvreg = func->nvreg;
    if (nvreg == 0)
        return xm_pass_no_change();

    bool any_inserted = false;

    for (uint32_t bi = 0; bi < func->nblk; bi++) {
        XmBlock *blk = func->blocks[bi];

// ---- Pass A: collect locally-allocated PTR vregs in this block ----
// tracked as (ref, alloc_ins_idx, skip) triples
#define ARC_MAX_LOCAL 64
        XmRef loc_ref[ARC_MAX_LOCAL];
        uint32_t loc_def[ARC_MAX_LOCAL];  // index of defining instruction
        bool loc_skip[ARC_MAX_LOCAL];
        uint32_t nloc = 0;

        for (uint32_t i = 0; i < blk->nins && nloc < ARC_MAX_LOCAL; i++) {
            XmIns *ins = &blk->ins[i];
            if (xm_ref_is_none(ins->dst) || !xm_ref_is_vreg(ins->dst))
                continue;
            uint32_t dvi = XM_REF_INDEX(ins->dst);
            if (dvi >= nvreg || dvi < func->num_params)
                continue;

            uint8_t vt = func->vregs[dvi].rep;
            if (vt != XR_REP_PTR && vt != XR_REP_TAGGED)
                continue;

            switch (ins->op) {
                case XM_ALLOC:
                case XM_RT_ARRAY_NEW:
                case XM_RT_MAP_NEW:
                case XM_CALL_C:
                case XM_CALL_C_LEAF:
                case XM_CALL:
                case XM_CALL_KNOWN:
                case XM_CALL_KNOWN_REG:
                case XM_CALL_SELF_DIRECT:
                    loc_ref[nloc] = ins->dst;
                    loc_def[nloc] = i;
                    loc_skip[nloc] = false;
                    nloc++;
                    break;
                default:
                    break;
            }
        }
        if (nloc == 0)
            continue;

        // ---- Pass B: mark escaping / ownership-transferred vregs ----

        // B1: escapes via successor phi → skip
        XmBlock *succs[2] = {blk->s1, blk->s2};
        for (int si = 0; si < 2; si++) {
            XmBlock *succ = succs[si];
            if (!succ)
                continue;
            for (XmPhi *phi = succ->phis; phi; phi = phi->next) {
                for (uint32_t pi = 0; pi < phi->narg; pi++) {
                    XmRef pa = phi->args[pi];
                    if (!xm_ref_is_vreg(pa))
                        continue;
                    for (uint32_t j = 0; j < nloc; j++) {
                        if (loc_ref[j] == pa) {
                            loc_skip[j] = true;
                            break;
                        }
                    }
                }
            }
        }

        // B2: escapes via RET value → skip
        if (blk->jmp.type == XM_JMP_RET && !xm_ref_is_none(blk->jmp.arg)) {
            XmRef rv = blk->jmp.arg;
            for (uint32_t j = 0; j < nloc; j++) {
                if (loc_ref[j] == rv) {
                    loc_skip[j] = true;
                    break;
                }
            }
        }

        // B3: ownership transferred to container via STORE_FIELD → skip
        for (uint32_t i = 0; i < blk->nins; i++) {
            XmIns *ins = &blk->ins[i];
            XmRef stored = XM_NONE;
            if (ins->op == XM_STORE_FIELD)
                stored = ins->args[1];
            else if (ins->op == XM_RT_INDEX_SET)
                stored = ins->args[1];
            else if (ins->op == XM_RT_ARRAY_PUSH)
                stored = ins->args[1];
            if (xm_ref_is_none(stored) || !xm_ref_is_vreg(stored))
                continue;
            for (uint32_t j = 0; j < nloc; j++) {
                if (loc_ref[j] == stored) {
                    loc_skip[j] = true;
                    break;
                }
            }
        }

        // ---- Pass C: find last-use index for each surviving vreg ----
        int32_t last_use[ARC_MAX_LOCAL];
        for (uint32_t j = 0; j < nloc; j++)
            last_use[j] = -1;

        for (uint32_t i = 0; i < blk->nins; i++) {
            XmIns *ins = &blk->ins[i];
            for (uint32_t j = 0; j < nloc; j++) {
                if (loc_skip[j])
                    continue;
                if (!xm_ref_uses_vreg(ins->args[0], ins->args[1], loc_ref[j]))
                    continue;
                last_use[j] = (int32_t) i;
                // Pool args: if this vreg is a call arg in the pool,
                // extend last_use to the CALL instruction itself.
                if (xm_ref_is_vreg(ins->dst)) {
                    uint32_t dvi = XM_REF_INDEX(ins->dst);
                    if (dvi < func->nvreg && func->vregs[dvi].call_nargs > 0) {
                        XmVReg *dvr = &func->vregs[dvi];
                        for (uint16_t pi = 0; pi < dvr->call_nargs; pi++) {
                            if (func->call_arg_pool[dvr->call_arg_start + pi] == loc_ref[j]) {
                                last_use[j] = (int32_t) i;
                                break;
                            }
                        }
                    }
                }
            }
        }
        // Also check JMP arg (e.g. BR condition)
        if (!xm_ref_is_none(blk->jmp.arg) && xm_ref_is_vreg(blk->jmp.arg)) {
            for (uint32_t j = 0; j < nloc; j++) {
                if (!loc_skip[j] && blk->jmp.arg == loc_ref[j])
                    last_use[j] = (int32_t) blk->nins;  // sentinel: after all insns
            }
        }

        // Count how many releases to insert per instruction slot
        // releases[i] = number of releases to insert *after* instruction i
        // (slot nins = after last instruction, before terminator)
        uint32_t slots = blk->nins + 1;  // +1 for "after last insn" sentinel
        uint8_t *releases_at = (uint8_t *) xr_calloc(slots, sizeof(uint8_t));
        if (!releases_at)
            continue;
        uint32_t total_releases = 0;

        for (uint32_t j = 0; j < nloc; j++) {
            if (loc_skip[j])
                continue;
            if (last_use[j] < 0) {
                // No use in this block: check if used in any other block.
                // If so, it is a cross-block live value — skip, don't release here.
                bool used_elsewhere = false;
                for (uint32_t ob = 0; ob < func->nblk && !used_elsewhere; ob++) {
                    if (ob == bi)
                        continue;
                    XmBlock *other = func->blocks[ob];
                    // Check other block's phis
                    for (XmPhi *phi = other->phis; phi && !used_elsewhere; phi = phi->next) {
                        for (uint32_t pi = 0; pi < phi->narg; pi++) {
                            if (phi->args[pi] == loc_ref[j]) {
                                used_elsewhere = true;
                                break;
                            }
                        }
                    }
                    // Check other block's instructions (including pool args)
                    for (uint32_t k = 0; k < other->nins && !used_elsewhere; k++) {
                        XmIns *oi = &other->ins[k];
                        if (xm_ref_uses_vreg(oi->args[0], oi->args[1], loc_ref[j]))
                            used_elsewhere = true;
                        if (!used_elsewhere && xm_ref_is_vreg(oi->dst)) {
                            uint32_t dvi2 = XM_REF_INDEX(oi->dst);
                            if (dvi2 < func->nvreg && func->vregs[dvi2].call_nargs > 0) {
                                XmVReg *dvr2 = &func->vregs[dvi2];
                                for (uint16_t pi2 = 0; pi2 < dvr2->call_nargs; pi2++) {
                                    if (func->call_arg_pool[dvr2->call_arg_start + pi2] ==
                                        loc_ref[j]) {
                                        used_elsewhere = true;
                                        break;
                                    }
                                }
                            }
                        }
                    }
                    // Check other block's JMP arg
                    if (!xm_ref_is_none(other->jmp.arg) && other->jmp.arg == loc_ref[j])
                        used_elsewhere = true;
                }
                if (used_elsewhere) {
                    loc_skip[j] = true;
                    continue;
                }

                // Truly unused after def in any block: release right after def
                uint32_t slot = loc_def[j] + 1;
                if (slot <= blk->nins) {
                    releases_at[slot]++;
                    total_releases++;
                }
                continue;
            }
            uint32_t slot = (uint32_t) (last_use[j] + 1);
            if (slot > blk->nins)
                slot = blk->nins;  // clamp to after-last-insn
            releases_at[slot]++;
            total_releases++;
        }
        (void) slots;

        if (total_releases == 0) {
            xr_free(releases_at);
            continue;
        }

        // ---- Pass D: rebuild instruction array with releases interleaved ----
        uint32_t new_nins = blk->nins + total_releases;
        XmIns *new_ins = (XmIns *) xm_arena_alloc(func->arena, new_nins * sizeof(XmIns));
        if (!new_ins)
            continue;

        uint32_t wi = 0;
        for (uint32_t i = 0; i <= blk->nins; i++) {
            // Copy original instruction at slot i (if < nins)
            if (i < blk->nins)
                new_ins[wi++] = blk->ins[i];

            // Insert releases scheduled after slot i
            if (releases_at[i] == 0)
                continue;
            for (uint32_t j = 0; j < nloc; j++) {
                if (loc_skip[j])
                    continue;
                uint32_t slot;
                if (last_use[j] < 0) {
                    slot = loc_def[j] + 1;
                    if (slot > blk->nins)
                        slot = blk->nins;
                } else {
                    slot = (uint32_t) (last_use[j] + 1);
                    if (slot > blk->nins)
                        slot = blk->nins;
                }
                if (slot != i)
                    continue;
                XmIns rel;
                memset(&rel, 0, sizeof(rel));
                rel.op = XM_RELEASE;
                rel.rep = XR_REP_VOID;
                rel.flags = XM_FLAG_SIDE_EFFECT;
                rel.dst = XM_NONE;
                rel.args[0] = loc_ref[j];
                rel.args[1] = XM_NONE;
                new_ins[wi++] = rel;
            }
        }

        XR_DCHECK(wi == new_nins, "instruction count mismatch after rewrite");
        blk->ins = new_ins;
        blk->nins = new_nins;
        xr_free(releases_at);
#undef ARC_MAX_LOCAL
    }

/* ================================================================
 * Phase B: cross-block release.
 *
 * For each alloc vreg whose def_block differs from its last_use_block,
 * insert a release in last_use_block immediately after the last use.
 *
 * Steps:
 *   1. Collect all alloc vregs across the function.
 *   2. For each, find the block that contains the last use.
 *   3. Skip if: escapes via phi, is RET value, ownership transferred.
 *   4. Insert release after last-use instruction in that block.
 * ================================================================ */

// Step 1: global alloc vreg scan
#define ARC_MAX_GLOBAL 256
    XmRef g_ref[ARC_MAX_GLOBAL];
    uint32_t g_def_blk[ARC_MAX_GLOBAL];  // block index where defined
    uint32_t g_def_ins[ARC_MAX_GLOBAL];  // instruction index within def block
    uint32_t ng = 0;

    for (uint32_t bi = 0; bi < func->nblk && ng < ARC_MAX_GLOBAL; bi++) {
        XmBlock *blk = func->blocks[bi];
        for (uint32_t i = 0; i < blk->nins && ng < ARC_MAX_GLOBAL; i++) {
            XmIns *ins = &blk->ins[i];
            if (xm_ref_is_none(ins->dst) || !xm_ref_is_vreg(ins->dst))
                continue;
            uint32_t dvi = XM_REF_INDEX(ins->dst);
            if (dvi >= nvreg || dvi < func->num_params)
                continue;
            uint8_t vt = func->vregs[dvi].rep;
            if (vt != XR_REP_PTR && vt != XR_REP_TAGGED)
                continue;
            switch (ins->op) {
                case XM_ALLOC:
                case XM_RT_ARRAY_NEW:
                case XM_RT_MAP_NEW:
                case XM_CALL_C:
                case XM_CALL_C_LEAF:
                case XM_CALL:
                case XM_CALL_KNOWN:
                case XM_CALL_KNOWN_REG:
                case XM_CALL_SELF_DIRECT:
                    g_ref[ng] = ins->dst;
                    g_def_blk[ng] = bi;
                    g_def_ins[ng] = i;
                    ng++;
                    break;
                default:
                    break;
            }
        }
    }

    // Step 2-4: per alloc vreg, find last_use_block and insert release
    for (uint32_t g = 0; g < ng; g++) {
        XmRef ref = g_ref[g];
        uint32_t def_bi = g_def_blk[g];

        // Find the block with the latest use of ref (across all blocks)
        // "latest" = deepest in RPO order; use rpo_id for comparison.
        int32_t best_lu_ins = -1;   // instruction index in best block
        uint32_t best_bi = def_bi;  // block index of best use

        for (uint32_t bi2 = 0; bi2 < func->nblk; bi2++) {
            XmBlock *blk2 = func->blocks[bi2];
            // Scan instructions for use
            int32_t lu = -1;
            for (uint32_t i = 0; i < blk2->nins; i++) {
                XmIns *ins = &blk2->ins[i];
                if (!xm_ref_uses_vreg(ins->args[0], ins->args[1], ref))
                    continue;
                lu = (int32_t) i;
            }
            // Check JMP arg
            if (!xm_ref_is_none(blk2->jmp.arg) && blk2->jmp.arg == ref)
                lu = (int32_t) blk2->nins;  // after last insn

            if (lu < 0)
                continue;

            // Choose block with higher RPO (later in execution order)
            if (best_lu_ins < 0 || blk2->rpo_id > func->blocks[best_bi]->rpo_id ||
                (bi2 == best_bi && lu > best_lu_ins)) {
                best_bi = bi2;
                best_lu_ins = lu;
            }
        }

        // Only handle cross-block case here; single-block already handled above
        if (best_bi == def_bi)
            continue;
        if (best_lu_ins < 0)
            continue;

        XmBlock *tblk = func->blocks[best_bi];

        // Skip if ref escapes via successor phi of target block
        bool skip = false;
        XmBlock *tsuccs[2] = {tblk->s1, tblk->s2};
        for (int si = 0; si < 2 && !skip; si++) {
            XmBlock *succ = tsuccs[si];
            if (!succ)
                continue;
            for (XmPhi *phi = succ->phis; phi && !skip; phi = phi->next) {
                for (uint32_t pi = 0; pi < phi->narg; pi++) {
                    if (phi->args[pi] == ref) {
                        skip = true;
                        break;
                    }
                }
            }
        }
        if (skip)
            continue;

        // Skip if ref is RET value of target block
        if (tblk->jmp.type == XM_JMP_RET && !xm_ref_is_none(tblk->jmp.arg) && tblk->jmp.arg == ref)
            continue;

        // Skip if ownership transferred via STORE_FIELD in any block
        bool transferred = false;
        for (uint32_t bi2 = 0; bi2 < func->nblk && !transferred; bi2++) {
            XmBlock *blk2 = func->blocks[bi2];
            for (uint32_t i = 0; i < blk2->nins && !transferred; i++) {
                XmIns *ins = &blk2->ins[i];
                XmRef stored = XM_NONE;
                if (ins->op == XM_STORE_FIELD)
                    stored = ins->args[1];
                else if (ins->op == XM_RT_INDEX_SET)
                    stored = ins->args[1];
                else if (ins->op == XM_RT_ARRAY_PUSH)
                    stored = ins->args[1];
                if (!xm_ref_is_none(stored) && stored == ref)
                    transferred = true;
            }
        }
        if (transferred)
            continue;

        // Insert a single release after best_lu_ins in tblk
        uint32_t slot = (uint32_t) (best_lu_ins + 1);
        if (slot > tblk->nins)
            slot = tblk->nins;

        uint32_t new_nins = tblk->nins + 1;
        XmIns *new_ins = (XmIns *) xm_arena_alloc(func->arena, new_nins * sizeof(XmIns));
        if (!new_ins)
            continue;

        // Copy instructions, inserting release at slot
        uint32_t wi = 0;
        for (uint32_t i = 0; i <= tblk->nins; i++) {
            if (i < tblk->nins)
                new_ins[wi++] = tblk->ins[i];
            if (i == slot) {
                XmIns rel;
                memset(&rel, 0, sizeof(rel));
                rel.op = XM_RELEASE;
                rel.rep = XR_REP_VOID;
                rel.flags = XM_FLAG_SIDE_EFFECT;
                rel.dst = XM_NONE;
                rel.args[0] = ref;
                rel.args[1] = XM_NONE;
                new_ins[wi++] = rel;
            }
        }
        XR_DCHECK(wi == new_nins, "instruction count mismatch after rewrite");
        tblk->ins = new_ins;
        tblk->nins = new_nins;
        any_inserted = true;
    }
#undef ARC_MAX_GLOBAL
    return any_inserted ? (XmPassChange) {false, true, false, 0, 0, 0} : xm_pass_no_change();
}

/* ========== Escape Analysis + Scalar Replacement ========== */

/*
 * Per-block escape analysis for XM_ALLOC instructions.
 *
 * Algorithm:
 *   1. Find XM_ALLOC in each block
 *   2. Check if the allocated object escapes:
 *      - returned via RET
 *      - passed to CALL_C / CALL (including call_arg_pool refs)
 *      - stored as a field value in another object (STORE_FIELD arg[1])
 *      - used in a phi node (cross-block live)
 *      - used by any instruction other than LOAD_FIELD/STORE_FIELD on self
 *   3. For non-escaping allocs, scalar-replace:
 *      - Create a vreg per field slot
 *      - STORE_FIELD(alloc, off, val) → MOV field_vreg[idx], val
 *      - LOAD_FIELD(alloc, off) → result = field_vreg[idx]
 *      - ALLOC → NOP (DCE will remove)
 */
/*
 * Maximum number of XrValue slots a single ALLOC may have before we
 * refuse scalar replacement.  Bounded for two reasons:
 *  1. We materialise one vreg per field in place; large allocations
 *     would inflate the register pressure dramatically.
 *  2. Deep heap shapes nearly always escape anyway, so the gain from
 *     replacing them is marginal.
 * The value is an engineering compromise, not a correctness limit.
 */
#define EA_MAX_SCALAR_FIELDS 64

typedef struct {
    XmRef ref;            // ALLOC result vreg
    uint32_t blk;         // containing block index
    uint32_t ins_idx;     // instruction index in that block
    int32_t alloc_size;   // allocation size
    int32_t base_offset;  // field base offset (GC header size)
    int32_t nfields;      // XrValue slot count
    bool escapes;         // final escape verdict
} EaAlloc;

/*
 * Collect every XM_ALLOC in the function, allocating on the heap so
 * there is no EA_MAX_ALLOC cap.  Returns number of entries; |*out|
 * receives the heap array (caller frees).
 */
static uint32_t ea_collect_allocs(XmFunc *func, EaAlloc **out) {
    uint32_t cap = 16, n = 0;
    EaAlloc *arr = (EaAlloc *) xr_malloc(cap * sizeof(EaAlloc));
    if (!arr) {
        *out = NULL;
        return 0;
    }

    for (uint32_t bi = 0; bi < func->nblk; bi++) {
        XmBlock *blk = func->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t i = 0; i < blk->nins; i++) {
            XmIns *ins = &blk->ins[i];
            if (ins->op != XM_ALLOC)
                continue;
            if (xm_ref_is_none(ins->dst) || !xm_ref_is_vreg(ins->dst))
                continue;

            int32_t asize = 0;
            if (xm_ref_is_const(ins->args[1])) {
                uint32_t ci = XM_REF_INDEX(ins->args[1]);
                if (ci < func->nconst)
                    asize = (int32_t) func->consts[ci].val.i64;
            }
            if (asize <= 16 || asize > 16 + EA_MAX_SCALAR_FIELDS * 16)
                continue;

            if (n >= cap) {
                cap *= 2;
                XR_REALLOC_OR_ABORT(arr, cap * sizeof(EaAlloc), "EA allocs");
            }
            arr[n].ref = ins->dst;
            arr[n].blk = bi;
            arr[n].ins_idx = i;
            arr[n].alloc_size = asize;
            arr[n].base_offset = 16;
            arr[n].nfields = (asize - 16) / 16;
            arr[n].escapes = false;
            n++;
        }
    }
    *out = arr;
    return n;
}

/*
 * Mark every allocation in |allocs| that escapes the current
 * function.  Walks the function once:
 *   - every instruction's args are inspected; a use that is neither a
 *     same-object LOAD_FIELD/STORE_FIELD/GUARD_SHAPE/BARRIER_FWD at
 *     args[0] escapes the allocation.
 *   - RET of an alloc vreg escapes.
 *   - A block's alloc being a PHI argument in any successor escapes.
 *   - A call_arg_pool reference to an alloc escapes.
 */
static void ea_mark_escapes(XmFunc *func, EaAlloc *allocs, uint32_t nalloc) {
    if (nalloc == 0)
        return;

    /* Build a quick alloc-ref → index map via a flat nvreg-sized table.
     * Allocs are identified by their dst vreg, which fits in an index
     * lookup without any hashing.  UINT32_MAX = not an ALLOC. */
    uint32_t *ref_to_alloc = (uint32_t *) xr_malloc(func->nvreg * sizeof(uint32_t));
    if (!ref_to_alloc)
        return;
    for (uint32_t v = 0; v < func->nvreg; v++)
        ref_to_alloc[v] = UINT32_MAX;
    for (uint32_t j = 0; j < nalloc; j++) {
        uint32_t v = XM_REF_INDEX(allocs[j].ref);
        if (v < func->nvreg)
            ref_to_alloc[v] = j;
    }

#define ESC_IF_ALLOC(ref)                                                                          \
    do {                                                                                           \
        if (xm_ref_is_vreg(ref)) {                                                                 \
            uint32_t _v = XM_REF_INDEX(ref);                                                       \
            if (_v < func->nvreg) {                                                                \
                uint32_t _j = ref_to_alloc[_v];                                                    \
                if (_j != UINT32_MAX)                                                              \
                    allocs[_j].escapes = true;                                                     \
            }                                                                                      \
        }                                                                                          \
    } while (0)

    /* Instruction-level scan: any use outside the "self as obj
     * argument of field ops" whitelist escapes. */
    for (uint32_t bi = 0; bi < func->nblk; bi++) {
        XmBlock *blk = func->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t i = 0; i < blk->nins; i++) {
            XmIns *ins = &blk->ins[i];
            for (int a = 0; a < 2; a++) {
                XmRef arg = ins->args[a];
                if (!xm_ref_is_vreg(arg))
                    continue;
                uint32_t v = XM_REF_INDEX(arg);
                if (v >= func->nvreg)
                    continue;
                uint32_t j = ref_to_alloc[v];
                if (j == UINT32_MAX)
                    continue;

                /* Whitelisted non-escaping uses: obj slot of
                 * LOAD_FIELD / STORE_FIELD / BARRIER_FWD. */
                if (a == 0 && (ins->op == XM_LOAD_FIELD || ins->op == XM_STORE_FIELD ||
                               ins->op == XM_BARRIER_FWD))
                    continue;
                allocs[j].escapes = true;
            }
        }

        /* RET of an alloc vreg escapes. */
        if (blk->jmp.type == XM_JMP_RET)
            ESC_IF_ALLOC(blk->jmp.arg);

        /* PHI arg in any successor escapes the value across the edge. */
        XmBlock *succs[2] = {blk->s1, blk->s2};
        for (int si = 0; si < 2; si++) {
            XmBlock *succ = succs[si];
            if (!succ)
                continue;
            for (XmPhi *phi = succ->phis; phi; phi = phi->next)
                for (uint32_t pi = 0; pi < phi->narg; pi++)
                    ESC_IF_ALLOC(phi->args[pi]);
        }
    }

    /* CALL arguments travel through call_arg_pool — scan them once. */
    if (func->call_arg_pool) {
        for (uint32_t v = 0; v < func->nvreg; v++) {
            if (func->vregs[v].call_nargs == 0)
                continue;
            uint32_t start = func->vregs[v].call_arg_start;
            for (uint16_t a = 0; a < func->vregs[v].call_nargs; a++)
                ESC_IF_ALLOC(func->call_arg_pool[start + a]);
        }
    }

#undef ESC_IF_ALLOC
    xr_free(ref_to_alloc);
}

/*
 * Scalar-replace a single non-escaping allocation.  Still block-local
 * — a full cross-block Mem2Reg would need Phi insertion at dom-tree
 * frontiers and is intentionally deferred.  Returns true when the
 * ALLOC was fully eliminated.
 */
static bool ea_scalar_replace(XmFunc *func, const EaAlloc *a) {
    XmBlock *blk = func->blocks[a->blk];
    if (!blk)
        return false;

    int nf = a->nfields;
    int base = a->base_offset;
    XmRef aref = a->ref;

    /* One synthetic vreg per field slot, bound to the XrValue-sized
     * storage the original ALLOC would have covered. */
    XmRef *field_vregs = (XmRef *) xr_malloc((size_t) nf * sizeof(XmRef));
    if (!field_vregs)
        return false;
    for (int f = 0; f < nf; f++)
        field_vregs[f] = xm_new_vreg(func, XR_REP_I64);

    for (uint32_t i = 0; i < blk->nins; i++) {
        XmIns *ins = &blk->ins[i];

        if (ins->op == XM_STORE_FIELD && xm_ref_is_vreg(ins->args[0]) && ins->args[0] == aref) {
            int32_t off = 0;
            if (xm_ref_is_const(ins->dst)) {
                uint32_t ci = XM_REF_INDEX(ins->dst);
                if (ci < func->nconst)
                    off = (int32_t) func->consts[ci].val.i64;
            }
            int fidx = (off - base) / 16;
            if (fidx >= 0 && fidx < nf) {
                XmRef val_ref = ins->args[1];
                if (xm_ref_is_vreg(val_ref)) {
                    uint32_t svi = XM_REF_INDEX(val_ref);
                    uint32_t dvi = XM_REF_INDEX(field_vregs[fidx]);
                    if (svi < func->nvreg && dvi < func->nvreg) {
                        force_vreg_vtag(func, dvi,
                                        type_kind_to_vtag(xm_ref_ctype(func, val_ref).kind));
                        func->vregs[dvi].heap_type = func->vregs[svi].heap_type;
                    }
                }
                ins->op = XM_MOV;
                ins->rep = XR_REP_I64;
                ins->dst = field_vregs[fidx];
                ins->args[0] = ins->args[1];
                ins->args[1] = XM_NONE;
                ins->flags = 0;
            }
        }

        if (ins->op == XM_LOAD_FIELD && xm_ref_is_vreg(ins->args[0]) && ins->args[0] == aref) {
            int32_t off = 0;
            if (xm_ref_is_const(ins->args[1])) {
                uint32_t ci = XM_REF_INDEX(ins->args[1]);
                if (ci < func->nconst)
                    off = (int32_t) func->consts[ci].val.i64;
            }
            int fidx = (off - base) / 16;
            if (fidx >= 0 && fidx < nf) {
                ins->op = XM_MOV;
                ins->rep = XR_REP_I64;
                ins->args[0] = field_vregs[fidx];
                ins->args[1] = XM_NONE;
            }
        }

        /* Object identity is gone, so the write barrier is vacuous on
         * the materialised scalar form. */
        if (ins->op == XM_BARRIER_FWD && xm_ref_is_vreg(ins->args[0]) && ins->args[0] == aref) {
            ins->op = XM_NOP;
            ins->dst = XM_NONE;
            ins->args[0] = XM_NONE;
            ins->args[1] = XM_NONE;
            ins->flags = 0;
        }
    }

    /* Retire the original ALLOC; DCE will remove the NOP later. */
    XmIns *alloc_ins = &blk->ins[a->ins_idx];
    alloc_ins->op = XM_NOP;
    alloc_ins->dst = XM_NONE;
    alloc_ins->args[0] = XM_NONE;
    alloc_ins->args[1] = XM_NONE;
    alloc_ins->flags = 0;

    xr_free(field_vregs);
    return true;
}

XmPassChange xm_pass_escape_analysis(XmFunc *func) {
    if (!func)
        return xm_pass_no_change();

    EaAlloc *allocs = NULL;
    uint32_t nalloc = ea_collect_allocs(func, &allocs);
    if (nalloc == 0) {
        xr_free(allocs);
        return xm_pass_no_change();
    }

    /* Single function-wide escape-set computation.  Cost is
     *   O(ninstr * args) + O(call_arg_pool) + O(nphi)
     * which beats the previous O(nalloc * nblk^2) Pass 2d handily. */
    ea_mark_escapes(func, allocs, nalloc);

    /* Block-local scalar replacement for survivors.  Cross-block
     * Mem2Reg is the obvious next step; it requires dom-frontier phi
     * insertion and will land in a follow-up once we have the general
     * helpers for it. */
    uint32_t n_replaced = 0;
    for (uint32_t j = 0; j < nalloc; j++) {
        if (!allocs[j].escapes)
            if (ea_scalar_replace(func, &allocs[j]))
                n_replaced++;
    }

    xr_free(allocs);
    return n_replaced ? (XmPassChange) {false, true, true, n_replaced, 0, 0} : xm_pass_no_change();
}

/*
 * Allocation Sinking: move XM_ALLOC from a dominating block into the
 * specific branch where it's actually used. If all uses of an allocation
 * are in blocks dominated by one successor of a BR, the ALLOC is sunk
 * to that successor's entry, avoiding allocation on the other path.
 *
 * This runs before escape_analysis so that sunk allocations can still
 * be scalar-replaced within their new (smaller) scope.
 */
XmPassChange xm_pass_alloc_sink(XmFunc *func) {
    if (!func || func->nblk < 3 || func->nvreg == 0)
        return xm_pass_no_change();

    uint32_t n_sunk = 0;

    uint32_t nblk = func->nblk;

    const XmDomTree *dt = xm_func_get_domtree(func);
    if (!dt)
        return xm_pass_no_change();

    for (uint32_t bi = 0; bi < nblk; bi++) {
        XmBlock *blk = func->blocks[bi];
        if (!blk || blk->nins == 0)
            continue;

        for (uint32_t ii = 0; ii < blk->nins; ii++) {
            XmIns *ins = &blk->ins[ii];
            if (ins->op != XM_ALLOC)
                continue;
            if (!xm_ref_is_vreg(ins->dst))
                continue;
            XmRef aref = ins->dst;

            // Collect all use blocks for this allocation
            uint32_t use_blocks[64];
            uint32_t nuse = 0;
            bool overflow = false;

            for (uint32_t ub = 0; ub < nblk && !overflow; ub++) {
                XmBlock *ublk = func->blocks[ub];
                if (!ublk)
                    continue;
                bool used_here = false;
                for (uint32_t k = 0; k < ublk->nins && !used_here; k++) {
                    if ((xm_ref_is_vreg(ublk->ins[k].args[0]) && ublk->ins[k].args[0] == aref) ||
                        (xm_ref_is_vreg(ublk->ins[k].args[1]) && ublk->ins[k].args[1] == aref))
                        used_here = true;
                }
                // Check phi args
                for (XmPhi *p = ublk->phis; p && !used_here; p = p->next) {
                    for (uint32_t a = 0; a < p->narg; a++) {
                        if (xm_ref_is_vreg(p->args[a]) && p->args[a] == aref) {
                            used_here = true;
                            break;
                        }
                    }
                }
                // Check terminator
                if (!used_here && xm_ref_is_vreg(ublk->jmp.arg) && ublk->jmp.arg == aref)
                    used_here = true;

                if (used_here) {
                    if (nuse < 64)
                        use_blocks[nuse++] = ub;
                    else
                        overflow = true;
                }
            }
            if (overflow || nuse == 0)
                continue;

            // All uses in the same block as the ALLOC: no sinking needed
            bool all_same = true;
            for (uint32_t u = 0; u < nuse; u++)
                if (use_blocks[u] != bi) {
                    all_same = false;
                    break;
                }
            if (all_same)
                continue;

            // Check if the ALLOC block has a BR with two successors
            if (blk->jmp.type != XM_JMP_BR || !blk->s1 || !blk->s2)
                continue;
            uint32_t s1_id = blk->s1->id;
            uint32_t s2_id = blk->s2->id;

            // Check if all uses are dominated by exactly one successor
            bool all_in_s1 = true, all_in_s2 = true;
            for (uint32_t u = 0; u < nuse; u++) {
                uint32_t ub = use_blocks[u];
                if (ub == bi) {
                    all_in_s1 = false;
                    all_in_s2 = false;
                    break;
                }
                if (!xm_dom_covers(dt, s1_id, ub))
                    all_in_s1 = false;
                if (!xm_dom_covers(dt, s2_id, ub))
                    all_in_s2 = false;
            }

            XmBlock *sink_target = NULL;
            if (all_in_s1 && !all_in_s2)
                sink_target = blk->s1;
            else if (all_in_s2 && !all_in_s1)
                sink_target = blk->s2;
            if (!sink_target)
                continue;

            // Sink: move ALLOC to the beginning of sink_target
            if (sink_target->nins < sink_target->ins_cap) {
                // Shift existing instructions right by 1
                memmove(&sink_target->ins[1], &sink_target->ins[0],
                        sink_target->nins * sizeof(XmIns));
                sink_target->ins[0] = *ins;
                sink_target->nins++;
                // NOP the original
                n_sunk++;
                ins->op = XM_NOP;
                ins->dst = XM_NONE;
                ins->args[0] = XM_NONE;
                ins->args[1] = XM_NONE;
                ins->flags = 0;
            }
        }
    }
    return n_sunk ? (XmPassChange) {false, true, false, n_sunk, 0, 0} : xm_pass_no_change();
}

/* ========== Automatic Function Inlining ========== */

/*
 * Scan for CALL_KNOWN and CALL_KNOWN_REG instructions and inline small callees.
 *
 * Multi-tier heuristics:
 *   Tier 1 (Tiny,  ≤5  bc): always inline (getter/setter)
 *   Tier 2 (Small, ≤15 bc): inline without inline_hint (utility functions)
 *   Tier 3 (Normal,≤30 bc): requires inline_hint >= 1
 *   Always: inline_hint == 2 bypasses size check (always_inline pragma)
 *   Loop bonus: size limits doubled for call sites inside loops
 *
 * Budget controls:
 *   - Max 8 inlinings per function (tiny inlinings exempt)
 *   - Caller budget: stop if total inlined bytecodes > 250
 *   - No upvalues / nested protos / generators
 *   - Not recursive (callee != caller proto)
 *
 * Call variants:
 *   CALL_KNOWN:     args[0]=const_ptr(proto), args[1]=const(nargs)
 *                   params in call_arg_pool (indexed by dst vreg)
 *   CALL_KNOWN_REG: args[0]=param0, args[1]=param1 (nargs ≤ 2)
 *                   proto stored in call_arg_pool slot 0
 */

// Extract a constant pointer from a CONST_I64 vreg chain
static void *extract_const_ptr_from_vreg(XmFunc *func, XmRef ref) {
    if (xm_ref_is_const(ref)) {
        uint32_t ci = XM_REF_INDEX(ref);
        if (ci < func->nconst)
            return func->consts[ci].val.ptr;
    }
    if (!xm_ref_is_vreg(ref))
        return NULL;
    uint32_t vi = XM_REF_INDEX(ref);
    if (vi >= func->nvreg)
        return NULL;
    XmIns *def = func->vregs[vi].def;
    if (!def || def->op != XM_CONST_I64)
        return NULL;
    if (xm_ref_is_const(def->args[0])) {
        uint32_t ci = XM_REF_INDEX(def->args[0]);
        if (ci < func->nconst)
            return func->consts[ci].val.ptr;
    }
    return NULL;
}

// Extract an integer constant from a ref (direct const or CONST_I64 vreg)
static bool extract_const_i64(XmFunc *func, XmRef ref, int64_t *out) {
    if (xm_ref_is_const(ref)) {
        uint32_t ci = XM_REF_INDEX(ref);
        if (ci < func->nconst) {
            *out = func->consts[ci].val.i64;
            return true;
        }
    }
    if (!xm_ref_is_vreg(ref))
        return false;
    uint32_t vi = XM_REF_INDEX(ref);
    if (vi >= func->nvreg)
        return false;
    XmIns *def = func->vregs[vi].def;
    if (!def || def->op != XM_CONST_I64)
        return false;
    if (xm_ref_is_const(def->args[0])) {
        uint32_t ci = XM_REF_INDEX(def->args[0]);
        if (ci < func->nconst) {
            *out = func->consts[ci].val.i64;
            return true;
        }
    }
    return false;
}

XmPassChange xm_pass_auto_inline(XmFunc *func, XrProto *caller_proto) {
    if (!func || !caller_proto)
        return xm_pass_no_change();

    int inlined_count = 0;
    int inlined_size = 0;              // total bytecodes inlined (caller budget)
    const int max_inline = 12;         // max non-tiny inlinings (up for recursive chains)
    const int max_inlined_size = 350;  // stop if caller grows too much

// Per-callee inline history: limits recursive/mutual-recursive depth
// and prevents excessive code bloat from repeated inlining of the same callee.
#define MAX_INLINE_CALLEES 16
    const int max_callee_depth = 3;  // max times a single callee can be inlined
    struct {
        XrProto *proto;
        int count;
    } callee_hist[MAX_INLINE_CALLEES];
    int n_callee_hist = 0;
    int rounds_since_sub_pipeline = 0;

    // Iterate blocks; inlining modifies block array so restart after each inline
    bool did_inline = true;
    while (did_inline && inlined_count < max_inline && inlined_size < max_inlined_size) {
        did_inline = false;

        for (uint32_t bi = 0; bi < func->nblk && !did_inline; bi++) {
            XmBlock *blk = func->blocks[bi];
            if (!blk)
                continue;

            for (uint32_t ii = 0; ii < blk->nins && !did_inline; ii++) {
                XmIns *ins = &blk->ins[ii];
                bool is_call_known = (ins->op == XM_CALL_KNOWN);
                bool is_call_reg = (ins->op == XM_CALL_KNOWN_REG);
                if (!is_call_known && !is_call_reg)
                    continue;

                // --- Extract callee proto and arguments ---
                XrProto *callee = NULL;
                int nargs = 0;
                XmRef call_args[8];
                for (int k = 0; k < 8; k++)
                    call_args[k] = XM_NONE;

                // Read call args from the call_arg_pool via dst vreg
                uint32_t dst_vi = xm_ref_is_vreg(ins->dst) ? XM_REF_INDEX(ins->dst) : func->nvreg;
                XmVReg *dst_vr = (dst_vi < func->nvreg) ? &func->vregs[dst_vi] : NULL;

                if (is_call_known) {
                    // CALL_KNOWN: args[0]=const_ptr(proto), args[1]=const(nargs)
                    callee = (XrProto *) extract_const_ptr_from_vreg(func, ins->args[0]);
                    if (!callee)
                        continue;

                    int64_t n = 0;
                    if (!extract_const_i64(func, ins->args[1], &n))
                        continue;
                    nargs = (int) n;
                    if (nargs < 0 || nargs > 7)
                        continue;

                    // Extract arguments from call_arg_pool
                    // Pool layout: [closure, param0, param1, ...] → params start at index 1
                    if (!dst_vr || dst_vr->call_nargs < (uint16_t) (1 + nargs))
                        continue;
                    for (int ai = 0; ai < nargs; ai++)
                        call_args[ai] = func->call_arg_pool[dst_vr->call_arg_start + 1 + ai];
                } else {
                    // CALL_KNOWN_REG: args[0]=param0, args[1]=param1
                    // Proto is in call_arg_pool slot 0.
                    // For CALL_KNOWN_REG, proto was stored by builder into jit_ctx->call_proto.
                    // Extract from pool slot 0 if available.
                    if (dst_vr && dst_vr->call_nargs > 0) {
                        XmRef proto_ref = func->call_arg_pool[dst_vr->call_arg_start];
                        callee = (XrProto *) extract_const_ptr_from_vreg(func, proto_ref);
                    }
                    if (!callee)
                        continue;

                    // Arguments directly from ins->args
                    if (!xm_ref_is_none(ins->args[0])) {
                        call_args[0] = ins->args[0];
                        nargs = 1;
                        if (!xm_ref_is_none(ins->args[1])) {
                            call_args[1] = ins->args[1];
                            nargs = 2;
                        }
                    }
                }

                // --- Heuristic checks ---
                int code_count = (int) PROTO_CODE_COUNT(callee);

                // Per-callee depth check: limits recursive and repeated inlining.
                // Each callee can be inlined at most max_callee_depth times.
                int callee_depth = 0;
                for (int k = 0; k < n_callee_hist; k++) {
                    if (callee_hist[k].proto == callee) {
                        callee_depth = callee_hist[k].count;
                        break;
                    }
                }
                if (callee_depth >= max_callee_depth)
                    continue;

                // Loop-depth bonus: calls inside loops are more profitable
                int depth_mult = (xm_block_loop_depth(func, blk->id) > 0) ? 2 : 1;

                // Multi-tier inline decision with loop bonus and depth penalty.
                // effective_size doubles per existing inline of this callee,
                // shrinking the threshold window for deeper recursive inlines.
                int effective_size = code_count << callee_depth;
                bool is_tiny = (effective_size <= 5 * depth_mult);
                bool should_inline;
                if (callee->inline_hint >= 2) {
                    should_inline = (effective_size <= 80 * depth_mult);
                } else if (is_tiny) {
                    should_inline = true;
                } else if (effective_size <= 15 * depth_mult) {
                    should_inline = true;
                } else if (effective_size <= 30 * depth_mult) {
                    should_inline = (callee->inline_hint >= 1);
                } else {
                    should_inline = false;
                }
                if (!should_inline)
                    continue;

                // Has upvalues: skip (upvalue capture is complex)
                if (PROTO_UPVAL_COUNT(callee) > 0)
                    continue;

                // Has nested protos (closures): skip
                if (PROTO_PROTO_COUNT(callee) > 0)
                    continue;

                // Generator or has defaults: skip
                if (callee->entry_type != 0)
                    continue;

                // --- Build callee Xm via xi_to_xm ---
                XmFunc *callee_func = NULL;
                if (callee->xi_func)
                    callee_func = xi_to_xm_lower((XiFunc *) callee->xi_func, callee,
                                                 (XiSlotMap *) callee->xi_slot_map, NULL, NULL);
                if (!callee_func)
                    continue;
                int max_blk = is_tiny ? 8 : 32;
                if (callee_func->nblk == 0 || callee_func->nblk > (uint32_t) max_blk) {
                    xm_func_destroy(callee_func);
                    continue;
                }

                // --- Inline ---
                XmRef result =
                    xm_inline_function(func, blk, ii, callee_func, call_args, (uint32_t) nargs);

                // If inline succeeded, replace uses of call result
                if (!xm_ref_is_none(result) && !xm_ref_is_none(ins->dst)) {
                    XmRef old_dst = ins->dst;
                    for (uint32_t rb = 0; rb < func->nblk; rb++) {
                        XmBlock *rblk = func->blocks[rb];
                        if (!rblk)
                            continue;
                        for (uint32_t ri = 0; ri < rblk->nins; ri++) {
                            XmIns *rins = &rblk->ins[ri];
                            if (xm_ref_is_vreg(rins->args[0]) && rins->args[0] == old_dst)
                                rins->args[0] = result;
                            if (xm_ref_is_vreg(rins->args[1]) && rins->args[1] == old_dst)
                                rins->args[1] = result;
                        }
                        if (xm_ref_is_vreg(rblk->jmp.arg) && rblk->jmp.arg == old_dst)
                            rblk->jmp.arg = result;
                        for (XmPhi *phi = rblk->phis; phi; phi = phi->next) {
                            for (uint32_t pi = 0; pi < phi->narg; pi++) {
                                if (phi->args[pi] == old_dst)
                                    phi->args[pi] = result;
                            }
                        }
                    }
                    if (func->call_arg_pool) {
                        for (uint32_t cv = 0; cv < func->nvreg; cv++) {
                            XmVReg *vr = &func->vregs[cv];
                            if (vr->call_nargs == 0)
                                continue;
                            for (uint16_t ca = 0; ca < vr->call_nargs; ca++) {
                                uint32_t pos = vr->call_arg_start + ca;
                                if (pos < func->call_arg_pool_used &&
                                    func->call_arg_pool[pos] == old_dst)
                                    func->call_arg_pool[pos] = result;
                            }
                        }
                    }
                    for (uint32_t di = 0; di < func->ndeopt; di++) {
                        XmDeoptInfo *info = &func->deopt_infos[di];
                        for (uint16_t ds = 0; ds < info->nslots; ds++) {
                            if (info->slots[ds].value == old_dst)
                                info->slots[ds].value = result;
                        }
                    }
                }

                // Propagate callee return type to inlined result vreg
                if (!xm_ref_is_none(result) && xm_ref_is_vreg(result)) {
                    uint32_t rvi = XM_REF_INDEX(result);
                    if (rvi < func->nvreg && !func->vregs[rvi].xrtype && callee->return_type_info)
                        func->vregs[rvi].xrtype = callee->return_type_info;
                }

                xm_rebuild_preds(func);
                xm_rebuild_vreg_defs(func);
                xm_func_destroy(callee_func);

                // Record callee in history for depth tracking
                bool found_hist = false;
                for (int k = 0; k < n_callee_hist; k++) {
                    if (callee_hist[k].proto == callee) {
                        callee_hist[k].count++;
                        found_hist = true;
                        break;
                    }
                }
                if (!found_hist && n_callee_hist < MAX_INLINE_CALLEES) {
                    callee_hist[n_callee_hist].proto = callee;
                    callee_hist[n_callee_hist].count = 1;
                    n_callee_hist++;
                }

                inlined_size += code_count;
                if (!is_tiny)
                    inlined_count++;
                did_inline = true;
                rounds_since_sub_pipeline++;
            }
        }

        // Lightweight sub-pipeline every 3 inlines to expose new call sites.
        // TypeProp + Specialize can turn polymorphic calls into CALL_KNOWN,
        // enabling cascading inlining including recursive cases.
        if (did_inline && rounds_since_sub_pipeline >= 3) {
            xm_pass_type_prop(func);
            xm_pass_specialize(func);
            xm_pass_dce(func);
            rounds_since_sub_pipeline = 0;
        }
    }

#undef MAX_INLINE_CALLEES
    return inlined_count > 0 ? (XmPassChange) {true, true, true, 0, 0, 0} : xm_pass_no_change();
}

// (INLINE_JIT_CALL_ARGS_BASE removed: args now come from call_arg_pool)

/* ========== Redundant Guard Elimination ========== */

/*
 * Eliminate duplicate guard instructions within each block.
 * When the same (vreg, expected_value) pair is checked by GUARD_TAG
 * or GUARD_SHAPE multiple times, the subsequent checks are redundant.
 * This is especially useful after guard hoisting gathers guards from
 * multiple loop iterations into the same preheader.
 */
XmPassChange xm_pass_elim_guards(XmFunc *func) {
    if (!func || func->nblk == 0)
        return xm_pass_no_change();

    uint32_t n_elim = 0;

#define GUARD_TABLE_SIZE 32
#define GUARD_TABLE_MASK (GUARD_TABLE_SIZE - 1)
    struct {
        XmRef arg0;
        XmRef arg1;
        uint16_t op;
        bool valid;
    } seen[GUARD_TABLE_SIZE];

    for (uint32_t bi = 0; bi < func->nblk; bi++) {
        XmBlock *blk = func->blocks[bi];
        memset(seen, 0, sizeof(seen));

        for (uint32_t i = 0; i < blk->nins; i++) {
            XmIns *ins = &blk->ins[i];

            if (ins->op != XM_GUARD_TAG && ins->op != XM_GUARD_NONNULL &&
                ins->op != XM_GUARD_CLASS && ins->op != XM_GUARD_KLASS)
                continue;

            /* Type-aware guard elimination: if type_prop already proved
             * the vreg has the guarded tag, the guard is redundant.
             * This is the cross-block counterpart of per-block dedup. */
            if (ins->op == XM_GUARD_TAG && xm_ref_is_vreg(ins->args[0])) {
                XmType gct = xm_ref_ctype(func, ins->args[0]);
                uint8_t gvtag = type_kind_to_vtag(gct.kind);
                if (gvtag != VTAG_TAGGED) {
                    // Check if guarded tag matches known tag
                    if (xm_ref_is_const(ins->args[1])) {
                        uint32_t ci = XM_REF_INDEX(ins->args[1]);
                        if (ci < func->nconst) {
                            uint8_t expected = (uint8_t) func->consts[ci].val.raw;
                            if (vtag_to_value_tag(gvtag) == expected) {
                                n_elim++;
                                ins->op = XM_NOP;
                                ins->dst = XM_NONE;
                                ins->args[0] = XM_NONE;
                                ins->args[1] = XM_NONE;
                                ins->flags = 0;
                                continue;
                            }
                        }
                    }
                }
            }

            // GUARD_NONNULL: eliminate if vreg is known PTR or fresh alloc
            if (ins->op == XM_GUARD_NONNULL && xm_ref_is_vreg(ins->args[0])) {
                uint32_t vi = XM_REF_INDEX(ins->args[0]);
                XmType gct = xm_ref_ctype(func, ins->args[0]);
                if (vi < func->nvreg &&
                    (xm_type_is_ptr(gct.kind) || func->vregs[vi].is_fresh_alloc)) {
                    n_elim++;
                    ins->op = XM_NOP;
                    ins->dst = XM_NONE;
                    ins->args[0] = XM_NONE;
                    ins->args[1] = XM_NONE;
                    ins->flags = 0;
                    continue;
                }
            }

            // GUARD_CLASS: eliminate if heap_type already known and matches
            if (ins->op == XM_GUARD_CLASS && xm_ref_is_vreg(ins->args[0])) {
                uint32_t vi = XM_REF_INDEX(ins->args[0]);
                if (vi < func->nvreg && func->vregs[vi].heap_type != 0) {
                    if (xm_ref_is_const(ins->args[1])) {
                        uint32_t ci = XM_REF_INDEX(ins->args[1]);
                        if (ci < func->nconst) {
                            uint16_t expected_ht = (uint16_t) func->consts[ci].val.raw;
                            if (func->vregs[vi].heap_type == expected_ht) {
                                n_elim++;
                                ins->op = XM_NOP;
                                ins->dst = XM_NONE;
                                ins->args[0] = XM_NONE;
                                ins->args[1] = XM_NONE;
                                ins->flags = 0;
                                continue;
                            }
                        }
                    }
                }
            }

            // Per-block dedup: hash the guard signature
            uint32_t h =
                ((uint32_t) ins->op * 31 + (uint32_t) ins->args[0] * 17 + (uint32_t) ins->args[1]) &
                GUARD_TABLE_MASK;

            bool found = false;
            for (uint32_t probe = 0; probe < GUARD_TABLE_SIZE; probe++) {
                uint32_t slot = (h + probe) & GUARD_TABLE_MASK;
                if (!seen[slot].valid) {
                    // Insert new guard
                    seen[slot].op = ins->op;
                    seen[slot].arg0 = ins->args[0];
                    seen[slot].arg1 = ins->args[1];
                    seen[slot].valid = true;
                    break;
                }
                if (seen[slot].op == ins->op && seen[slot].arg0 == ins->args[0] &&
                    seen[slot].arg1 == ins->args[1]) {
                    found = true;
                    break;
                }
            }

            if (found) {
                // Duplicate guard — convert to NOP
                n_elim++;
                ins->op = XM_NOP;
                ins->dst = XM_NONE;
                ins->args[0] = XM_NONE;
                ins->args[1] = XM_NONE;
                ins->flags = 0;
            }
        }
    }
#undef GUARD_TABLE_SIZE
#undef GUARD_TABLE_MASK
    return n_elim ? (XmPassChange) {false, true, false, n_elim, 0, 0} : xm_pass_no_change();
}

/* ========== Branch Value Propagation (propjnz) ========== */

/*
 * Propagate known values through conditional branches.
 *
 * When BR(cond) branches:
 *   1. False successor (single-pred): cond is known to be 0
 *   2. If cond = EQ(a, K): true successor (single-pred): a == K
 *   3. If cond = NE(a, K): false successor (single-pred): a == K
 *
 * This enables subsequent passes (const_prop, DCE) to simplify code
 * in branches where values are constrained by the branch condition.
 */

/* Helper: replace all uses of old_ref with new_ref in a block's instructions,
 * phis, and terminator. Returns true if any replacement was made. */
static bool propjnz_replace_in_block(XmBlock *blk, XmRef old_ref, XmRef new_ref) {
    bool changed = false;

    for (uint32_t i = 0; i < blk->nins; i++) {
        XmIns *ins = &blk->ins[i];
        for (int a = 0; a < 2; a++) {
            if (xm_ref_is_vreg(ins->args[a]) && ins->args[a] == old_ref) {
                ins->args[a] = new_ref;
                changed = true;
            }
        }
    }

    /* Don't replace in phi args — they correspond to values from predecessors,
     * not values computed within this block */

    if (xm_ref_is_vreg(blk->jmp.arg) && blk->jmp.arg == old_ref) {
        blk->jmp.arg = new_ref;
        changed = true;
    }

    return changed;
}

// Helper: check if a vreg is defined by a CONST_I64 and return its value
static bool propjnz_is_const(XmFunc *func, XmRef ref, int64_t *val) {
    if (!xm_ref_is_vreg(ref))
        return false;
    uint32_t idx = XM_REF_INDEX(ref);
    if (idx >= func->nvreg)
        return false;
    XmIns *def = func->vregs[idx].def;
    if (!def || def->op != XM_CONST_I64)
        return false;
    if (!xm_ref_is_const(def->args[0]))
        return false;
    uint32_t ci = XM_REF_INDEX(def->args[0]);
    if (ci >= func->nconst)
        return false;
    *val = func->consts[ci].val.i64;
    return true;
}

XmPassChange xm_pass_propjnz(XmFunc *func) {
    if (!func || func->nblk < 2)
        return xm_pass_no_change();

    bool any_prop = false;

    for (uint32_t bi = 0; bi < func->nblk; bi++) {
        XmBlock *blk = func->blocks[bi];
        if (blk->jmp.type != XM_JMP_BR)
            continue;

        XmRef cond = blk->jmp.arg;
        if (!xm_ref_is_vreg(cond))
            continue;

        XmBlock *s_true = blk->s1;   // taken when cond != 0
        XmBlock *s_false = blk->s2;  // taken when cond == 0
        if (!s_true || !s_false)
            continue;

        /* Rule 1: In false successor (single-pred), cond is 0.
         * Replace uses of cond with a zero constant. */
        if (s_false->npred == 1) {
            XmRef zero_vreg = XM_NONE;
            // Search for an existing vreg defined as CONST_I64(0)
            for (uint32_t v = 0; v < func->nvreg; v++) {
                int64_t val;
                XmRef vref = XM_REF(XM_REF_VREG, v);
                if (propjnz_is_const(func, vref, &val) && val == 0) {
                    zero_vreg = vref;
                    break;
                }
            }
            if (!xm_ref_is_none(zero_vreg)) {
                propjnz_replace_in_block(s_false, cond, zero_vreg);
                any_prop = true;
            }
        }

        // Find the defining instruction of cond
        uint32_t cond_idx = XM_REF_INDEX(cond);
        if (cond_idx >= func->nvreg)
            continue;
        XmIns *def = func->vregs[cond_idx].def;
        if (!def)
            continue;

        // Rule 2: cond = EQ(a, K) → true successor: a == K
        if (def->op == XM_EQ && s_true->npred == 1) {
            XmRef a = def->args[0];
            XmRef b = def->args[1];
            int64_t val;
            if (xm_ref_is_vreg(a) && propjnz_is_const(func, b, &val)) {
                propjnz_replace_in_block(s_true, a, b);
                any_prop = true;
            } else if (xm_ref_is_vreg(b) && propjnz_is_const(func, a, &val)) {
                propjnz_replace_in_block(s_true, b, a);
                any_prop = true;
            }
        }

        // Rule 3: cond = NE(a, K) → false successor: a == K
        if (def->op == XM_NE && s_false->npred == 1) {
            XmRef a = def->args[0];
            XmRef b = def->args[1];
            int64_t val;
            if (xm_ref_is_vreg(a) && propjnz_is_const(func, b, &val)) {
                propjnz_replace_in_block(s_false, a, b);
                any_prop = true;
            } else if (xm_ref_is_vreg(b) && propjnz_is_const(func, a, &val)) {
                propjnz_replace_in_block(s_false, b, a);
                any_prop = true;
            }
        }
    }
    return any_prop ? (XmPassChange) {false, false, true, 0, 0, 0} : xm_pass_no_change();
}

/* ========== Global Code Motion (GCM) ========== */

/*
 * Click's GCM algorithm adapted for Xm:
 *   1. Early schedule: place each movable instruction as early as possible
 *      (deepest dominator where all operands are available)
 *   2. Late schedule: find LCA of all use blocks in the dominator tree
 *   3. Best placement: between early and late, pick the block with the
 *      lowest loop depth to minimize execution frequency
 *   4. Move instructions to their best blocks
 *   5. Re-order within each block via DFS to ensure def-before-use
 *
 * Only pure instructions are moved. Pinned instructions (loads, stores,
 * guards, calls, side-effecting ops) stay in their original block.
 */

#define GCM_NOBID UINT32_MAX

static bool gcm_is_pinned(uint16_t op) {
    return !xm_op_is_pure(op);
}

// Compute dominator depth for each block
static void gcm_fill_depth(uint32_t *idom, uint32_t *depth, uint32_t nblk) {
    for (uint32_t i = 0; i < nblk; i++)
        depth[i] = 0;
    bool changed = true;
    while (changed) {
        changed = false;
        for (uint32_t i = 1; i < nblk; i++) {
            if (idom[i] < nblk && depth[i] != depth[idom[i]] + 1) {
                depth[i] = depth[idom[i]] + 1;
                changed = true;
            }
        }
    }
}

// LCA in dominator tree
static uint32_t gcm_lca(uint32_t *idom, uint32_t *depth, uint32_t a, uint32_t b) {
    if (a == GCM_NOBID)
        return b;
    if (b == GCM_NOBID)
        return a;
    while (a != b) {
        while (depth[a] > depth[b]) {
            if (a == 0)
                return 0;
            a = idom[a];
        }
        while (depth[b] > depth[a]) {
            if (b == 0)
                return 0;
            b = idom[b];
        }
        if (a == b)
            break;
        if (a == 0 || b == 0)
            return 0;
        a = idom[a];
        b = idom[b];
    }
    return a;
}

// Early schedule: find earliest possible block for vreg v
static uint32_t gcm_early(XmFunc *func, uint32_t *idom, uint32_t *depth, uint32_t *early,
                          uint32_t *def_blk, uint32_t v) {
    if (early[v] != GCM_NOBID)
        return early[v];

    XmIns *def = func->vregs[v].def;
    if (!def) {
        // Phi-defined or no def: stays in defining block
        early[v] = def_blk[v];
        return early[v];
    }

    // Find the deepest dominator where both operands are available
    early[v] = 0;  // mark visiting (entry block)
    uint32_t best = 0;
    for (int a = 0; a < 2; a++) {
        if (!xm_ref_is_vreg(def->args[a]))
            continue;
        uint32_t av = XM_REF_INDEX(def->args[a]);
        if (av >= func->nvreg)
            continue;
        uint32_t ab = gcm_early(func, idom, depth, early, def_blk, av);
        if (ab != GCM_NOBID && depth[ab] > depth[best])
            best = ab;
    }

    if (gcm_is_pinned(def->op)) {
        // Pinned instructions cannot move
        early[v] = def_blk[v];
    } else {
        early[v] = best;
    }
    return early[v];
}

XmPassChange xm_pass_gcm(XmFunc *func) {
    if (!func || func->nblk < 2 || func->nvreg == 0)
        return xm_pass_no_change();
    if (func->nblk > XM_MAX_FUNC_BLOCKS || func->nvreg > XM_MAX_FUNC_VREGS)
        return xm_pass_no_change();

    uint32_t nblk = func->nblk;
    uint32_t nv = func->nvreg;

    // Step 0: Build infrastructure (domtree is cached in func)
    const XmDomTree *dt = xm_func_get_domtree(func);
    if (!dt)
        return xm_pass_no_change();
    uint32_t *idom = dt->idom;

    uint32_t *depth = (uint32_t *) xr_malloc(nblk * sizeof(uint32_t));
    uint32_t *early = (uint32_t *) xr_malloc(nv * sizeof(uint32_t));
    uint32_t *best = (uint32_t *) xr_malloc(nv * sizeof(uint32_t));
    uint32_t *def_blk = (uint32_t *) xr_calloc(nv, sizeof(uint32_t));
    if (!depth || !early || !best || !def_blk) {
        xr_free(depth);
        xr_free(early);
        xr_free(best);
        xr_free(def_blk);
        return xm_pass_no_change();
    }
    gcm_fill_depth(idom, depth, nblk);

    // Build def_blk map for each vreg
    for (uint32_t v = 0; v < nv; v++) {
        early[v] = GCM_NOBID;
        best[v] = GCM_NOBID;
    }
    for (uint32_t bi = 0; bi < nblk; bi++) {
        XmBlock *blk = func->blocks[bi];
        for (XmPhi *p = blk->phis; p; p = p->next) {
            if (xm_ref_is_vreg(p->dst)) {
                uint32_t v = XM_REF_INDEX(p->dst);
                if (v < nv)
                    def_blk[v] = bi;
            }
        }
        for (uint32_t i = 0; i < blk->nins; i++) {
            XmIns *ins = &blk->ins[i];
            if (xm_ref_is_vreg(ins->dst)) {
                uint32_t v = XM_REF_INDEX(ins->dst);
                if (v < nv) {
                    def_blk[v] = bi;
                    func->vregs[v].def = ins;
                }
            }
        }
    }

    // Step 1: Early schedule
    for (uint32_t v = 0; v < nv; v++)
        gcm_early(func, idom, depth, early, def_blk, v);

    // Step 2: Late schedule — find LCA of all use blocks
    uint32_t *late = best;  // reuse array
    for (uint32_t v = 0; v < nv; v++)
        late[v] = GCM_NOBID;

    for (uint32_t bi = 0; bi < nblk; bi++) {
        XmBlock *blk = func->blocks[bi];
        // Instruction arg uses
        for (uint32_t i = 0; i < blk->nins; i++) {
            XmIns *ins = &blk->ins[i];
            for (int a = 0; a < 2; a++) {
                if (!xm_ref_is_vreg(ins->args[a]))
                    continue;
                uint32_t v = XM_REF_INDEX(ins->args[a]);
                if (v < nv)
                    late[v] = gcm_lca(idom, depth, late[v], bi);
            }
        }
        // Phi arg uses: use belongs to the predecessor block
        for (XmPhi *p = blk->phis; p; p = p->next) {
            for (uint32_t a = 0; a < p->narg && a < blk->npred; a++) {
                if (!xm_ref_is_vreg(p->args[a]))
                    continue;
                uint32_t v = XM_REF_INDEX(p->args[a]);
                if (v < nv && blk->preds[a])
                    late[v] = gcm_lca(idom, depth, late[v], blk->preds[a]->id);
            }
        }
        // Terminator arg use
        if (xm_ref_is_vreg(blk->jmp.arg)) {
            uint32_t v = XM_REF_INDEX(blk->jmp.arg);
            if (v < nv)
                late[v] = gcm_lca(idom, depth, late[v], bi);
        }
    }

    // Step 3: Best placement — walk early→late, pick min loop_depth
    uint32_t moved = 0;
    for (uint32_t v = 0; v < nv; v++) {
        XmIns *def = func->vregs[v].def;
        if (!def || gcm_is_pinned(def->op))
            continue;
        uint32_t e = early[v];
        uint32_t l = late[v];
        if (e == GCM_NOBID || l == GCM_NOBID)
            continue;
        if (e == def_blk[v] && l == def_blk[v])
            continue;

        // Verify early dominates late
        if (!xm_dom_covers(dt, e, l))
            continue;

        // Walk from late up to early, find best loop depth
        uint32_t best_bid = l;
        int best_depth = (int) xm_block_loop_depth(func, func->blocks[l]->id);
        uint32_t cur = l;
        uint32_t limit = nblk;
        while (cur != e && limit-- > 0) {
            cur = idom[cur];
            if (cur >= nblk)
                break;
            int ld = (int) xm_block_loop_depth(func, func->blocks[cur]->id);
            if (ld < best_depth) {
                best_depth = ld;
                best_bid = cur;
            }
        }

        // Only move if the best block differs from current
        if (best_bid != def_blk[v]) {
            best[v] = best_bid;
            moved++;
        } else {
            best[v] = GCM_NOBID;
        }
    }

    if (moved == 0) {
        xr_free(depth);
        xr_free(early);
        xr_free(best);
        xr_free(def_blk);
        return xm_pass_no_change();
    }

    /* Step 4: Move instructions to their best blocks.
     * NOP the instruction in the original block and append to the target. */
    for (uint32_t v = 0; v < nv; v++) {
        if (best[v] == GCM_NOBID)
            continue;
        XmIns *def = func->vregs[v].def;
        if (!def)
            continue;

        // Skip pinned instructions — best[] aliases late[] so pinned
        // ops may retain a stale late-schedule value from step 2.
        if (gcm_is_pinned(def->op))
            continue;

        XmBlock *target = func->blocks[best[v]];
        // Append instruction to target block
        if (target->nins < target->ins_cap) {
            target->ins[target->nins] = *def;
            func->vregs[v].def = &target->ins[target->nins];
            def_blk[v] = best[v];
            target->nins++;
            // NOP the original
            def->op = XM_NOP;
            def->dst = XM_NONE;
            def->args[0] = XM_NONE;
            def->args[1] = XM_NONE;
        }
    }

    /* Step 5: Block-internal DFS scheduling to fix def-before-use.
     * After moving instructions, a use might precede its definition
     * within the same block. Topological sort via DFS fixes this. */
    {
        // Build vreg→instruction-index-in-block map for scheduling
        XmIns *tmp_ins = (XmIns *) xr_malloc(256 * sizeof(XmIns));
        uint8_t *scheduled = (uint8_t *) xr_calloc(nv, sizeof(uint8_t));
        if (tmp_ins && scheduled) {
            for (uint32_t bi = 0; bi < nblk; bi++) {
                XmBlock *blk = func->blocks[bi];
                if (blk->nins <= 1)
                    continue;

                uint32_t out = 0;
                uint32_t cap = 256;
                memset(scheduled, 0, nv);

                /* Simple topological sort: for each instruction in order,
                 * ensure its operand defs (if in same block) are emitted first */
                for (uint32_t i = 0; i < blk->nins && out < cap; i++) {
                    XmIns *ins = &blk->ins[i];
                    if (ins->op == XM_NOP)
                        continue;

                    // Check if operands need to be scheduled first
                    for (int a = 0; a < 2; a++) {
                        if (!xm_ref_is_vreg(ins->args[a]))
                            continue;
                        uint32_t av = XM_REF_INDEX(ins->args[a]);
                        if (av >= nv || scheduled[av] || def_blk[av] != bi)
                            continue;
                        // Find and emit the operand's definition
                        for (uint32_t j = i + 1; j < blk->nins; j++) {
                            if (xm_ref_is_vreg(blk->ins[j].dst) &&
                                XM_REF_INDEX(blk->ins[j].dst) == av && blk->ins[j].op != XM_NOP) {
                                if (out < cap)
                                    tmp_ins[out++] = blk->ins[j];
                                blk->ins[j].op = XM_NOP;
                                scheduled[av] = 1;
                                break;
                            }
                        }
                    }

                    if (ins->op != XM_NOP && out < cap) {
                        tmp_ins[out++] = *ins;
                        if (xm_ref_is_vreg(ins->dst)) {
                            uint32_t dv = XM_REF_INDEX(ins->dst);
                            if (dv < nv)
                                scheduled[dv] = 1;
                        }
                    }
                }

                // Copy back
                if (out > 0 && out <= blk->ins_cap) {
                    memcpy(blk->ins, tmp_ins, out * sizeof(XmIns));
                    blk->nins = out;
                }
            }
        }
        xr_free(tmp_ins);
        xr_free(scheduled);
    }

    xr_free(depth);
    xr_free(early);
    xr_free(best);
    xr_free(def_blk);
    return (XmPassChange) {false, true, false, 0, 0, 0};
}

/* ========== Type Propagation Pass ========== */
