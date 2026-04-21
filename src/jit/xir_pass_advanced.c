/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_pass_advanced.c - XIR advanced optimization passes
 *
 * KEY CONCEPT:
 *   Function inlining, write barriers, ARC releases, escape analysis,
 *   scalar replacement, auto-inlining, redundant guard elimination,
 *   branch value propagation, global code motion, and type propagation.
 */

#include "xir_pass_internal.h"
#include "xir_pass_limits.h"
#include "xir_domtree.h"
#include "xir_looptree.h"
#include "../base/xchecks.h"

// Forward declarations for type helpers used across passes
static void refine_vreg_vtag(XirFunc *func, uint32_t vi, uint8_t vtag);
static void force_vreg_vtag(XirFunc *func, uint32_t vi, uint8_t vtag);

/* ========== Function Inlining ========== */

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
XirRef xir_inline_function(XirFunc *caller, XirBlock *call_block,
                           uint32_t call_ins_idx, XirFunc *callee,
                           XirRef *call_args, uint32_t nargs) {
    if (!caller || !callee || !call_block) return XIR_NONE;
    if (callee->nblk == 0) return XIR_NONE;

    // --- Step 1: Remap tables ---
    // Vreg remap: callee vreg index → new caller vreg ref
    uint32_t callee_nvreg = callee->nvreg;
    XirRef *vreg_map = (XirRef *)xr_malloc(callee_nvreg * sizeof(XirRef));
    if (!vreg_map) return XIR_NONE;
    for (uint32_t i = 0; i < callee_nvreg; i++) vreg_map[i] = XIR_NONE;

    // Map callee params (vreg 0..nargs-1) to call arguments
    uint32_t map_count = (nargs < callee_nvreg) ? nargs : callee_nvreg;
    for (uint32_t i = 0; i < map_count; i++) {
        vreg_map[i] = call_args[i];
    }

    // Allocate new vregs for non-param callee vregs (propagate metadata)
    for (uint32_t i = nargs; i < callee_nvreg; i++) {
        vreg_map[i] = xir_new_vreg(caller, callee->vregs[i].rep);
        if (xir_ref_is_vreg(vreg_map[i])) {
            uint32_t vi = XIR_REF_INDEX(vreg_map[i]);
            if (vi < caller->nvreg) {
                caller->vregs[vi].heap_type = callee->vregs[i].heap_type;
                caller->vregs[vi].xrtype = callee->vregs[i].xrtype;
                caller->vregs[vi].callee_proto = callee->vregs[i].callee_proto;
                caller->vregs[vi].shape_hint = callee->vregs[i].shape_hint;
                caller->vregs[vi].layout = callee->vregs[i].layout;
                caller->vregs[vi].struct_idx = callee->vregs[i].struct_idx;
            }
        }
    }

    // Const remap: callee const index → new caller const ref
    uint32_t nconst = callee->nconst;
    XirRef *const_map = (XirRef *)xr_malloc(nconst * sizeof(XirRef));
    if (!const_map) { xr_free(vreg_map); return XIR_NONE; }
    for (uint32_t i = 0; i < nconst; i++) {
        XirConst *c = &callee->consts[i];
        if (c->rep == XR_REP_F64) {
            const_map[i] = xir_const_f64(caller, c->val.f64);
        } else {
            const_map[i] = xir_const_i64(caller, c->val.i64);
        }
    }

    // Helper: remap a single XirRef from callee space to caller space
    #define REMAP_REF(r) do { \
        if (xir_ref_is_vreg(r)) { \
            uint32_t _idx = XIR_REF_INDEX(r); \
            if (_idx < callee_nvreg) (r) = vreg_map[_idx]; \
        } else if (xir_ref_is_const(r)) { \
            uint32_t _idx = XIR_REF_INDEX(r); \
            if (_idx < nconst) (r) = const_map[_idx]; \
        } \
    } while(0)

    // --- Step 2: Split call_block ---
    // Create continuation block (instructions after the call)
    XirBlock *cont_blk = xir_func_add_block(caller, "inline.cont");

    // Move instructions after call_ins_idx to cont_blk
    uint32_t post_start = call_ins_idx + 1;
    for (uint32_t i = post_start; i < call_block->nins; i++) {
        XirIns *src = &call_block->ins[i];
        xir_emit_raw(caller, cont_blk, src->op, src->rep,
                     src->dst, src->args[0], src->args[1]);
        cont_blk->ins[cont_blk->nins - 1].flags = src->flags;
        cont_blk->ins[cont_blk->nins - 1].ctype = src->ctype;
    }

    // Copy terminator to cont_blk
    cont_blk->jmp = call_block->jmp;
    cont_blk->s1 = call_block->s1;
    cont_blk->s2 = call_block->s2;

    // Truncate call_block (remove the call instruction and everything after)
    call_block->nins = call_ins_idx;

    // --- Step 3: Clone callee blocks ---
    XirBlock **cloned_blocks = (XirBlock **)xr_malloc(callee->nblk * sizeof(XirBlock *));
    if (!cloned_blocks) { xr_free(vreg_map); xr_free(const_map); return XIR_NONE; }

    for (uint32_t bi = 0; bi < callee->nblk; bi++) {
        char label[64];
        snprintf(label, sizeof(label), "inline.%s.%u",
                 callee->blocks[bi]->label ? callee->blocks[bi]->label : "b", bi);
        cloned_blocks[bi] = xir_func_add_block(caller, label);
    }

    // Collect return blocks and their return values for Phi
    XirBlock *ret_blocks[32];
    XirRef    ret_values[32];
    uint32_t  nret = 0;

    for (uint32_t bi = 0; bi < callee->nblk; bi++) {
        XirBlock *src_blk = callee->blocks[bi];
        XirBlock *dst_blk = cloned_blocks[bi];

        // Clone instructions with remapping
        for (uint32_t i = 0; i < src_blk->nins; i++) {
            XirIns ins = src_blk->ins[i];  // copy
            // Remap dst and args
            if (xir_ref_is_vreg(ins.dst)) {
                uint32_t idx = XIR_REF_INDEX(ins.dst);
                if (idx < callee_nvreg) ins.dst = vreg_map[idx];
            }
            REMAP_REF(ins.args[0]);
            REMAP_REF(ins.args[1]);

            xir_emit_raw(caller, dst_blk, ins.op, ins.rep,
                         ins.dst, ins.args[0], ins.args[1]);
            dst_blk->ins[dst_blk->nins - 1].flags = ins.flags;
            dst_blk->ins[dst_blk->nins - 1].ctype = ins.ctype;
        }

        // Clone terminator
        switch (src_blk->jmp.type) {
            case XIR_JMP_JMP: {
                uint32_t target_id = src_blk->s1->id;
                if (target_id >= callee->nblk) break;
                xir_block_set_jmp(dst_blk, cloned_blocks[target_id]);
                xir_block_add_pred(cloned_blocks[target_id], dst_blk, caller->arena);
                break;
            }
            case XIR_JMP_BR: {
                XirRef cond = src_blk->jmp.arg;
                REMAP_REF(cond);
                uint32_t s1_id = src_blk->s1->id;
                uint32_t s2_id = src_blk->s2->id;
                if (s1_id >= callee->nblk || s2_id >= callee->nblk) break;
                xir_block_set_br(dst_blk, cond,
                                 cloned_blocks[s1_id], cloned_blocks[s2_id]);
                xir_block_add_pred(cloned_blocks[s1_id], dst_blk, caller->arena);
                xir_block_add_pred(cloned_blocks[s2_id], dst_blk, caller->arena);
                break;
            }
            case XIR_JMP_RET: {
                // Replace RET with JMP to continuation
                XirRef ret_val = src_blk->jmp.arg;
                REMAP_REF(ret_val);
                xir_block_set_jmp(dst_blk, cont_blk);
                xir_block_add_pred(cont_blk, dst_blk, caller->arena);
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
        for (XirPhi *phi = src_blk->phis; phi; phi = phi->next) {
            XirPhi *new_phi = xir_add_phi(caller, dst_blk, phi->rep);
            // Remap phi dst
            uint32_t phi_dst_idx = XIR_REF_INDEX(phi->dst);
            if (phi_dst_idx < callee_nvreg) {
                new_phi->dst = vreg_map[phi_dst_idx];
            }
            // Remap phi args (need to match remapped preds)
            for (uint32_t p = 0; p < phi->narg; p++) {
                XirRef arg = phi->args[p];
                REMAP_REF(arg);
                xir_phi_set_arg(new_phi, p, arg);
            }
        }
    }

    #undef REMAP_REF

    // --- Step 4: Wire call_block → callee entry ---
    xir_block_set_jmp(call_block, cloned_blocks[0]);
    xir_block_add_pred(cloned_blocks[0], call_block, caller->arena);

    // --- Step 5: Create return value Phi in cont_blk ---
    XirRef result_ref = XIR_NONE;
    if (nret == 1) {
        // Single return: no Phi needed, just use the value directly
        result_ref = ret_values[0];
    } else if (nret > 1) {
        // Multiple return paths: create Phi in cont_blk
        uint8_t ret_type = (callee->proto && callee->proto->return_type_info)
            ? xr_type_rep(callee->proto->return_type_info) : XR_REP_TAGGED;
        if (ret_type == XR_REP_TAGGED) ret_type = XR_REP_I64;
        XirPhi *ret_phi = xir_add_phi(caller, cont_blk, ret_type);
        for (uint32_t i = 0; i < nret; i++) {
            // Find pred index for this return block
            for (uint32_t p = 0; p < cont_blk->npred; p++) {
                if (cont_blk->preds[p] == ret_blocks[i]) {
                    xir_phi_set_arg(ret_phi, p, ret_values[i]);
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
 * Scan XIR for pointer store operations and insert write barrier instructions.
 * - XIR_STORE_FIELD → insert XIR_BARRIER_FWD (forward barrier: mark child)
 *
 * This pass rebuilds each block's instruction array with barriers interleaved.
 */
/*
 * Check if a STORE_FIELD needs a write barrier.
 * Uses ins->rep (xr_tag) first; for RUNTIME (0xFF), consults ctype.
 */
static bool sf_needs_barrier(XirFunc *func, XirIns *ins) {
    // Freshly allocated containers are guaranteed WHITE — no barrier needed
    if (ins->flags & XIR_FLAG_NO_BARRIER) return false;

    uint8_t tag = ins->rep;
    if (tag == XR_TAG_PTR) return true;
    if (tag != 0xFF /* XIR_SF_TAG_RUNTIME */) return false;

    // RUNTIME: consult value ctype for a more precise decision
    XirRef val = ins->args[1];
    if (xir_ref_is_vreg(val)) {
        uint32_t vi = XIR_REF_INDEX(val);
        if (vi < func->nvreg) {
            XirType vct = xir_ref_ctype(func, val);
            uint8_t vk = type_kind_to_vtag(vct.kind);
            if (vk != VTAG_TAGGED) {
                return (vk == VTAG_PTR);
            }
            // UNKNOWN tag but machine type may help
            uint8_t mt = func->vregs[vi].rep;
            return (mt == XR_REP_PTR || mt == XR_REP_TAGGED);
        }
    }
    return true; // conservative
}

void xir_insert_write_barriers(XirFunc *func) {
    if (!func || !func->arena) return;

    for (uint32_t bi = 0; bi < func->nblk; bi++) {
        XirBlock *blk = func->blocks[bi];

        uint32_t barrier_count = 0;
        for (uint32_t i = 0; i < blk->nins; i++) {
            if (blk->ins[i].op == XIR_STORE_FIELD &&
                sf_needs_barrier(func, &blk->ins[i]))
                barrier_count++;
        }
        if (barrier_count == 0) continue;

        uint32_t new_nins = blk->nins + barrier_count;
        XirIns *new_ins = (XirIns *)xir_arena_alloc(func->arena,
                                                      new_nins * sizeof(XirIns));
        if (!new_ins) continue;

        uint32_t j = 0;
        for (uint32_t i = 0; i < blk->nins; i++) {
            new_ins[j++] = blk->ins[i];

            if (blk->ins[i].op == XIR_STORE_FIELD &&
                sf_needs_barrier(func, &blk->ins[i])) {
                XirIns barrier;
                memset(&barrier, 0, sizeof(barrier));
                barrier.op    = XIR_BARRIER_FWD;
                barrier.rep  = XR_REP_VOID;
                barrier.flags = XIR_FLAG_SIDE_EFFECT;
                barrier.dst   = XIR_NONE;
                barrier.args[0] = blk->ins[i].args[0];  // parent
                barrier.args[1] = blk->ins[i].args[1];  // child/value
                new_ins[j++] = barrier;
            }
        }

        XR_DCHECK(j == new_nins, "instruction count mismatch after rewrite");
        blk->ins  = new_ins;
        blk->nins = new_nins;
    }
}

/* ========== InsertArcReleases (AOT only) ========== */

/*
 * Per-block, per-vreg last-use release insertion.
 *
 * For each basic block, for each PTR/TAGGED vreg allocated in that block
 * (by ALLOC, RT_ARRAY_NEW, RT_MAP_NEW, or any CALL returning PTR):
 *
 *   1. Find the last instruction in the block that uses the vreg.
 *   2. Insert XIR_RELEASE immediately after that instruction.
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
static bool xir_ref_uses_vreg(XirRef a0, XirRef a1, XirRef r) {
    return (xir_ref_is_vreg(a0) && a0 == r) ||
           (xir_ref_is_vreg(a1) && a1 == r);
}

void xir_insert_arc_releases(XirFunc *func) {
    if (!func || !func->arena) return;

    uint32_t nvreg = func->nvreg;
    if (nvreg == 0) return;

    for (uint32_t bi = 0; bi < func->nblk; bi++) {
        XirBlock *blk = func->blocks[bi];

        // ---- Pass A: collect locally-allocated PTR vregs in this block ----
        // tracked as (ref, alloc_ins_idx, skip) triples
        #define ARC_MAX_LOCAL 64
        XirRef  loc_ref[ARC_MAX_LOCAL];
        uint32_t loc_def[ARC_MAX_LOCAL];   // index of defining instruction
        bool     loc_skip[ARC_MAX_LOCAL];
        uint32_t nloc = 0;

        for (uint32_t i = 0; i < blk->nins && nloc < ARC_MAX_LOCAL; i++) {
            XirIns *ins = &blk->ins[i];
            if (xir_ref_is_none(ins->dst) || !xir_ref_is_vreg(ins->dst)) continue;
            uint32_t dvi = XIR_REF_INDEX(ins->dst);
            if (dvi >= nvreg || dvi < func->num_params) continue;

            uint8_t vt = func->vregs[dvi].rep;
            if (vt != XR_REP_PTR && vt != XR_REP_TAGGED) continue;

            switch (ins->op) {
                case XIR_ALLOC:
                case XIR_RT_ARRAY_NEW:
                case XIR_RT_MAP_NEW:
                case XIR_CALL_C:
                case XIR_CALL_C_LEAF:
                case XIR_CALL:
                case XIR_CALL_KNOWN:
                case XIR_CALL_KNOWN_REG:
                case XIR_CALL_SELF_DIRECT:
                    loc_ref[nloc]  = ins->dst;
                    loc_def[nloc]  = i;
                    loc_skip[nloc] = false;
                    nloc++;
                    break;
                default: break;
            }
        }
        if (nloc == 0) continue;

        // ---- Pass B: mark escaping / ownership-transferred vregs ----

        // B1: escapes via successor phi → skip
        XirBlock *succs[2] = { blk->s1, blk->s2 };
        for (int si = 0; si < 2; si++) {
            XirBlock *succ = succs[si];
            if (!succ) continue;
            for (XirPhi *phi = succ->phis; phi; phi = phi->next) {
                for (uint32_t pi = 0; pi < phi->narg; pi++) {
                    XirRef pa = phi->args[pi];
                    if (!xir_ref_is_vreg(pa)) continue;
                    for (uint32_t j = 0; j < nloc; j++) {
                        if (loc_ref[j] == pa) { loc_skip[j] = true; break; }
                    }
                }
            }
        }

        // B2: escapes via RET value → skip
        if (blk->jmp.type == XIR_JMP_RET && !xir_ref_is_none(blk->jmp.arg)) {
            XirRef rv = blk->jmp.arg;
            for (uint32_t j = 0; j < nloc; j++) {
                if (loc_ref[j] == rv) { loc_skip[j] = true; break; }
            }
        }

        // B3: ownership transferred to container via STORE_FIELD → skip
        for (uint32_t i = 0; i < blk->nins; i++) {
            XirIns *ins = &blk->ins[i];
            XirRef stored = XIR_NONE;
            if (ins->op == XIR_STORE_FIELD)    stored = ins->args[1];
            else if (ins->op == XIR_RT_INDEX_SET) stored = ins->args[1];
            else if (ins->op == XIR_RT_ARRAY_PUSH) stored = ins->args[1];
            if (xir_ref_is_none(stored) || !xir_ref_is_vreg(stored)) continue;
            for (uint32_t j = 0; j < nloc; j++) {
                if (loc_ref[j] == stored) { loc_skip[j] = true; break; }
            }
        }

        // ---- Pass C: find last-use index for each surviving vreg ----
        int32_t last_use[ARC_MAX_LOCAL];
        for (uint32_t j = 0; j < nloc; j++) last_use[j] = -1;

        for (uint32_t i = 0; i < blk->nins; i++) {
            XirIns *ins = &blk->ins[i];
            for (uint32_t j = 0; j < nloc; j++) {
                if (loc_skip[j]) continue;
                if (!xir_ref_uses_vreg(ins->args[0], ins->args[1], loc_ref[j]))
                    continue;
                last_use[j] = (int32_t)i;
                // Pool args: if this vreg is a call arg in the pool,
                // extend last_use to the CALL instruction itself.
                if (xir_ref_is_vreg(ins->dst)) {
                    uint32_t dvi = XIR_REF_INDEX(ins->dst);
                    if (dvi < func->nvreg && func->vregs[dvi].call_nargs > 0) {
                        XirVReg *dvr = &func->vregs[dvi];
                        for (uint16_t pi = 0; pi < dvr->call_nargs; pi++) {
                            if (func->call_arg_pool[dvr->call_arg_start + pi] == loc_ref[j]) {
                                last_use[j] = (int32_t)i;
                                break;
                            }
                        }
                    }
                }
            }
        }
        // Also check JMP arg (e.g. BR condition)
        if (!xir_ref_is_none(blk->jmp.arg) && xir_ref_is_vreg(blk->jmp.arg)) {
            for (uint32_t j = 0; j < nloc; j++) {
                if (!loc_skip[j] && blk->jmp.arg == loc_ref[j])
                    last_use[j] = (int32_t)blk->nins;  // sentinel: after all insns
            }
        }

        // Count how many releases to insert per instruction slot
        // releases[i] = number of releases to insert *after* instruction i
        // (slot nins = after last instruction, before terminator)
        uint32_t slots = blk->nins + 1;  // +1 for "after last insn" sentinel
        uint8_t releases_at[blk->nins + 1];
        memset(releases_at, 0, sizeof(releases_at));
        uint32_t total_releases = 0;

        for (uint32_t j = 0; j < nloc; j++) {
            if (loc_skip[j]) continue;
            if (last_use[j] < 0) {
                // No use in this block: check if used in any other block.
                // If so, it is a cross-block live value — skip, don't release here.
                bool used_elsewhere = false;
                for (uint32_t ob = 0; ob < func->nblk && !used_elsewhere; ob++) {
                    if (ob == bi) continue;
                    XirBlock *other = func->blocks[ob];
                    // Check other block's phis
                    for (XirPhi *phi = other->phis; phi && !used_elsewhere; phi = phi->next) {
                        for (uint32_t pi = 0; pi < phi->narg; pi++) {
                            if (phi->args[pi] == loc_ref[j]) { used_elsewhere = true; break; }
                        }
                    }
                    // Check other block's instructions (including pool args)
                    for (uint32_t k = 0; k < other->nins && !used_elsewhere; k++) {
                        XirIns *oi = &other->ins[k];
                        if (xir_ref_uses_vreg(oi->args[0], oi->args[1], loc_ref[j]))
                            used_elsewhere = true;
                        if (!used_elsewhere && xir_ref_is_vreg(oi->dst)) {
                            uint32_t dvi2 = XIR_REF_INDEX(oi->dst);
                            if (dvi2 < func->nvreg && func->vregs[dvi2].call_nargs > 0) {
                                XirVReg *dvr2 = &func->vregs[dvi2];
                                for (uint16_t pi2 = 0; pi2 < dvr2->call_nargs; pi2++) {
                                    if (func->call_arg_pool[dvr2->call_arg_start + pi2] == loc_ref[j]) {
                                        used_elsewhere = true; break;
                                    }
                                }
                            }
                        }
                    }
                    // Check other block's JMP arg
                    if (!xir_ref_is_none(other->jmp.arg) &&
                        other->jmp.arg == loc_ref[j]) used_elsewhere = true;
                }
                if (used_elsewhere) { loc_skip[j] = true; continue; }

                // Truly unused after def in any block: release right after def
                uint32_t slot = loc_def[j] + 1;
                if (slot <= blk->nins) { releases_at[slot]++; total_releases++; }
                continue;
            }
            uint32_t slot = (uint32_t)(last_use[j] + 1);
            if (slot > blk->nins) slot = blk->nins;  // clamp to after-last-insn
            releases_at[slot]++;
            total_releases++;
        }
        (void)slots;

        if (total_releases == 0) continue;

        // ---- Pass D: rebuild instruction array with releases interleaved ----
        uint32_t new_nins = blk->nins + total_releases;
        XirIns *new_ins = (XirIns *)xir_arena_alloc(func->arena,
                                                      new_nins * sizeof(XirIns));
        if (!new_ins) continue;

        uint32_t wi = 0;
        for (uint32_t i = 0; i <= blk->nins; i++) {
            // Copy original instruction at slot i (if < nins)
            if (i < blk->nins) new_ins[wi++] = blk->ins[i];

            // Insert releases scheduled after slot i
            if (releases_at[i] == 0) continue;
            for (uint32_t j = 0; j < nloc; j++) {
                if (loc_skip[j]) continue;
                uint32_t slot;
                if (last_use[j] < 0) {
                    slot = loc_def[j] + 1;
                    if (slot > blk->nins) slot = blk->nins;
                } else {
                    slot = (uint32_t)(last_use[j] + 1);
                    if (slot > blk->nins) slot = blk->nins;
                }
                if (slot != i) continue;
                XirIns rel;
                memset(&rel, 0, sizeof(rel));
                rel.op      = XIR_RELEASE;
                rel.rep    = XR_REP_VOID;
                rel.flags   = XIR_FLAG_SIDE_EFFECT;
                rel.dst     = XIR_NONE;
                rel.args[0] = loc_ref[j];
                rel.args[1] = XIR_NONE;
                new_ins[wi++] = rel;
            }
        }

        XR_DCHECK(wi == new_nins, "instruction count mismatch after rewrite");
        blk->ins  = new_ins;
        blk->nins = new_nins;
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
    XirRef  g_ref[ARC_MAX_GLOBAL];
    uint32_t g_def_blk[ARC_MAX_GLOBAL];   // block index where defined
    uint32_t g_def_ins[ARC_MAX_GLOBAL];   // instruction index within def block
    uint32_t ng = 0;

    for (uint32_t bi = 0; bi < func->nblk && ng < ARC_MAX_GLOBAL; bi++) {
        XirBlock *blk = func->blocks[bi];
        for (uint32_t i = 0; i < blk->nins && ng < ARC_MAX_GLOBAL; i++) {
            XirIns *ins = &blk->ins[i];
            if (xir_ref_is_none(ins->dst) || !xir_ref_is_vreg(ins->dst)) continue;
            uint32_t dvi = XIR_REF_INDEX(ins->dst);
            if (dvi >= nvreg || dvi < func->num_params) continue;
            uint8_t vt = func->vregs[dvi].rep;
            if (vt != XR_REP_PTR && vt != XR_REP_TAGGED) continue;
            switch (ins->op) {
                case XIR_ALLOC:
                case XIR_RT_ARRAY_NEW:
                case XIR_RT_MAP_NEW:
                case XIR_CALL_C:
                case XIR_CALL_C_LEAF:
                case XIR_CALL:
                case XIR_CALL_KNOWN:
                case XIR_CALL_KNOWN_REG:
                case XIR_CALL_SELF_DIRECT:
                    g_ref[ng]     = ins->dst;
                    g_def_blk[ng] = bi;
                    g_def_ins[ng] = i;
                    ng++;
                    break;
                default: break;
            }
        }
    }

    // Step 2-4: per alloc vreg, find last_use_block and insert release
    for (uint32_t g = 0; g < ng; g++) {
        XirRef ref = g_ref[g];
        uint32_t def_bi = g_def_blk[g];

        // Find the block with the latest use of ref (across all blocks)
        // "latest" = deepest in RPO order; use rpo_id for comparison.
        int32_t  best_lu_ins = -1;    // instruction index in best block
        uint32_t best_bi     = def_bi; // block index of best use

        for (uint32_t bi2 = 0; bi2 < func->nblk; bi2++) {
            XirBlock *blk2 = func->blocks[bi2];
            // Scan instructions for use
            int32_t lu = -1;
            for (uint32_t i = 0; i < blk2->nins; i++) {
                XirIns *ins = &blk2->ins[i];
                if (!xir_ref_uses_vreg(ins->args[0], ins->args[1], ref)) continue;
                lu = (int32_t)i;
            }
            // Check JMP arg
            if (!xir_ref_is_none(blk2->jmp.arg) && blk2->jmp.arg == ref)
                lu = (int32_t)blk2->nins; // after last insn

            if (lu < 0) continue;

            // Choose block with higher RPO (later in execution order)
            if (best_lu_ins < 0 ||
                blk2->rpo_id > func->blocks[best_bi]->rpo_id ||
                (bi2 == best_bi && lu > best_lu_ins)) {
                best_bi     = bi2;
                best_lu_ins = lu;
            }
        }

        // Only handle cross-block case here; single-block already handled above
        if (best_bi == def_bi) continue;
        if (best_lu_ins < 0) continue;

        XirBlock *tblk = func->blocks[best_bi];

        // Skip if ref escapes via successor phi of target block
        bool skip = false;
        XirBlock *tsuccs[2] = { tblk->s1, tblk->s2 };
        for (int si = 0; si < 2 && !skip; si++) {
            XirBlock *succ = tsuccs[si];
            if (!succ) continue;
            for (XirPhi *phi = succ->phis; phi && !skip; phi = phi->next) {
                for (uint32_t pi = 0; pi < phi->narg; pi++) {
                    if (phi->args[pi] == ref) { skip = true; break; }
                }
            }
        }
        if (skip) continue;

        // Skip if ref is RET value of target block
        if (tblk->jmp.type == XIR_JMP_RET &&
            !xir_ref_is_none(tblk->jmp.arg) && tblk->jmp.arg == ref) continue;

        // Skip if ownership transferred via STORE_FIELD in any block
        bool transferred = false;
        for (uint32_t bi2 = 0; bi2 < func->nblk && !transferred; bi2++) {
            XirBlock *blk2 = func->blocks[bi2];
            for (uint32_t i = 0; i < blk2->nins && !transferred; i++) {
                XirIns *ins = &blk2->ins[i];
                XirRef stored = XIR_NONE;
                if (ins->op == XIR_STORE_FIELD)     stored = ins->args[1];
                else if (ins->op == XIR_RT_INDEX_SET) stored = ins->args[1];
                else if (ins->op == XIR_RT_ARRAY_PUSH) stored = ins->args[1];
                if (!xir_ref_is_none(stored) && stored == ref) transferred = true;
            }
        }
        if (transferred) continue;

        // Insert a single release after best_lu_ins in tblk
        uint32_t slot = (uint32_t)(best_lu_ins + 1);
        if (slot > tblk->nins) slot = tblk->nins;

        uint32_t new_nins = tblk->nins + 1;
        XirIns *new_ins = (XirIns *)xir_arena_alloc(func->arena,
                                                      new_nins * sizeof(XirIns));
        if (!new_ins) continue;

        // Copy instructions, inserting release at slot
        uint32_t wi = 0;
        for (uint32_t i = 0; i <= tblk->nins; i++) {
            if (i < tblk->nins) new_ins[wi++] = tblk->ins[i];
            if (i == slot) {
                XirIns rel;
                memset(&rel, 0, sizeof(rel));
                rel.op      = XIR_RELEASE;
                rel.rep    = XR_REP_VOID;
                rel.flags   = XIR_FLAG_SIDE_EFFECT;
                rel.dst     = XIR_NONE;
                rel.args[0] = ref;
                rel.args[1] = XIR_NONE;
                new_ins[wi++] = rel;
            }
        }
        XR_DCHECK(wi == new_nins, "instruction count mismatch after rewrite");
        tblk->ins  = new_ins;
        tblk->nins = new_nins;
    }
    #undef ARC_MAX_GLOBAL
}

/* ========== Escape Analysis + Scalar Replacement ========== */

/*
 * Per-block escape analysis for XIR_ALLOC instructions.
 *
 * Algorithm:
 *   1. Find XIR_ALLOC in each block
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
    XirRef   ref;           // ALLOC result vreg
    uint32_t blk;            // containing block index
    uint32_t ins_idx;        // instruction index in that block
    int32_t  alloc_size;     // allocation size
    int32_t  base_offset;    // field base offset (GC header size)
    int32_t  nfields;        // XrValue slot count
    bool     escapes;        // final escape verdict
} EaAlloc;

/*
 * Collect every XIR_ALLOC in the function, allocating on the heap so
 * there is no EA_MAX_ALLOC cap.  Returns number of entries; |*out|
 * receives the heap array (caller frees).
 */
static uint32_t ea_collect_allocs(XirFunc *func, EaAlloc **out) {
    uint32_t cap = 16, n = 0;
    EaAlloc *arr = (EaAlloc *)xr_malloc(cap * sizeof(EaAlloc));
    if (!arr) { *out = NULL; return 0; }

    for (uint32_t bi = 0; bi < func->nblk; bi++) {
        XirBlock *blk = func->blocks[bi];
        if (!blk) continue;
        for (uint32_t i = 0; i < blk->nins; i++) {
            XirIns *ins = &blk->ins[i];
            if (ins->op != XIR_ALLOC) continue;
            if (xir_ref_is_none(ins->dst) || !xir_ref_is_vreg(ins->dst)) continue;

            int32_t asize = 0;
            if (xir_ref_is_const(ins->args[1])) {
                uint32_t ci = XIR_REF_INDEX(ins->args[1]);
                if (ci < func->nconst)
                    asize = (int32_t)func->consts[ci].val.i64;
            }
            if (asize <= 16 || asize > 16 + EA_MAX_SCALAR_FIELDS * 16) continue;

            if (n >= cap) {
                cap *= 2;
                XR_REALLOC_OR_ABORT(arr, cap * sizeof(EaAlloc),
                                    "EA allocs");
            }
            arr[n].ref         = ins->dst;
            arr[n].blk         = bi;
            arr[n].ins_idx     = i;
            arr[n].alloc_size  = asize;
            arr[n].base_offset = 16;
            arr[n].nfields     = (asize - 16) / 16;
            arr[n].escapes     = false;
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
static void ea_mark_escapes(XirFunc *func, EaAlloc *allocs, uint32_t nalloc) {
    if (nalloc == 0) return;

    /* Build a quick alloc-ref → index map via a flat nvreg-sized table.
     * Allocs are identified by their dst vreg, which fits in an index
     * lookup without any hashing.  UINT32_MAX = not an ALLOC. */
    uint32_t *ref_to_alloc = (uint32_t *)xr_malloc(func->nvreg * sizeof(uint32_t));
    if (!ref_to_alloc) return;
    for (uint32_t v = 0; v < func->nvreg; v++) ref_to_alloc[v] = UINT32_MAX;
    for (uint32_t j = 0; j < nalloc; j++) {
        uint32_t v = XIR_REF_INDEX(allocs[j].ref);
        if (v < func->nvreg) ref_to_alloc[v] = j;
    }

    #define ESC_IF_ALLOC(ref) do {                              \
        if (xir_ref_is_vreg(ref)) {                              \
            uint32_t _v = XIR_REF_INDEX(ref);                    \
            if (_v < func->nvreg) {                              \
                uint32_t _j = ref_to_alloc[_v];                  \
                if (_j != UINT32_MAX) allocs[_j].escapes = true; \
            }                                                    \
        }                                                        \
    } while (0)

    /* Instruction-level scan: any use outside the "self as obj
     * argument of field ops" whitelist escapes. */
    for (uint32_t bi = 0; bi < func->nblk; bi++) {
        XirBlock *blk = func->blocks[bi];
        if (!blk) continue;
        for (uint32_t i = 0; i < blk->nins; i++) {
            XirIns *ins = &blk->ins[i];
            for (int a = 0; a < 2; a++) {
                XirRef arg = ins->args[a];
                if (!xir_ref_is_vreg(arg)) continue;
                uint32_t v = XIR_REF_INDEX(arg);
                if (v >= func->nvreg) continue;
                uint32_t j = ref_to_alloc[v];
                if (j == UINT32_MAX) continue;

                /* Whitelisted non-escaping uses: obj slot of
                 * LOAD_FIELD / STORE_FIELD / GUARD_SHAPE / BARRIER_FWD. */
                if (a == 0 && (ins->op == XIR_LOAD_FIELD ||
                               ins->op == XIR_STORE_FIELD ||
                               ins->op == XIR_GUARD_SHAPE ||
                               ins->op == XIR_BARRIER_FWD))
                    continue;
                allocs[j].escapes = true;
            }
        }

        /* RET of an alloc vreg escapes. */
        if (blk->jmp.type == XIR_JMP_RET)
            ESC_IF_ALLOC(blk->jmp.arg);

        /* PHI arg in any successor escapes the value across the edge. */
        XirBlock *succs[2] = { blk->s1, blk->s2 };
        for (int si = 0; si < 2; si++) {
            XirBlock *succ = succs[si];
            if (!succ) continue;
            for (XirPhi *phi = succ->phis; phi; phi = phi->next)
                for (uint32_t pi = 0; pi < phi->narg; pi++)
                    ESC_IF_ALLOC(phi->args[pi]);
        }
    }

    /* CALL arguments travel through call_arg_pool — scan them once. */
    if (func->call_arg_pool) {
        for (uint32_t v = 0; v < func->nvreg; v++) {
            if (func->vregs[v].call_nargs == 0) continue;
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
static bool ea_scalar_replace(XirFunc *func, const EaAlloc *a) {
    XirBlock *blk = func->blocks[a->blk];
    if (!blk) return false;

    int nf = a->nfields;
    int base = a->base_offset;
    XirRef aref = a->ref;

    /* One synthetic vreg per field slot, bound to the XrValue-sized
     * storage the original ALLOC would have covered. */
    XirRef *field_vregs = (XirRef *)xr_malloc((size_t)nf * sizeof(XirRef));
    if (!field_vregs) return false;
    for (int f = 0; f < nf; f++)
        field_vregs[f] = xir_new_vreg(func, XR_REP_I64);

    for (uint32_t i = 0; i < blk->nins; i++) {
        XirIns *ins = &blk->ins[i];

        if (ins->op == XIR_STORE_FIELD &&
            xir_ref_is_vreg(ins->args[0]) && ins->args[0] == aref) {
            int32_t off = 0;
            if (xir_ref_is_const(ins->dst)) {
                uint32_t ci = XIR_REF_INDEX(ins->dst);
                if (ci < func->nconst)
                    off = (int32_t)func->consts[ci].val.i64;
            }
            int fidx = (off - base) / 16;
            if (fidx >= 0 && fidx < nf) {
                XirRef val_ref = ins->args[1];
                if (xir_ref_is_vreg(val_ref)) {
                    uint32_t svi = XIR_REF_INDEX(val_ref);
                    uint32_t dvi = XIR_REF_INDEX(field_vregs[fidx]);
                    if (svi < func->nvreg && dvi < func->nvreg) {
                        force_vreg_vtag(func, dvi,
                            type_kind_to_vtag(xir_ref_ctype(func, val_ref).kind));
                        func->vregs[dvi].heap_type = func->vregs[svi].heap_type;
                    }
                }
                ins->op = XIR_MOV;
                ins->rep = XR_REP_I64;
                ins->dst = field_vregs[fidx];
                ins->args[0] = ins->args[1];
                ins->args[1] = XIR_NONE;
                ins->flags = 0;
            }
        }

        if (ins->op == XIR_LOAD_FIELD &&
            xir_ref_is_vreg(ins->args[0]) && ins->args[0] == aref) {
            int32_t off = 0;
            if (xir_ref_is_const(ins->args[1])) {
                uint32_t ci = XIR_REF_INDEX(ins->args[1]);
                if (ci < func->nconst)
                    off = (int32_t)func->consts[ci].val.i64;
            }
            int fidx = (off - base) / 16;
            if (fidx >= 0 && fidx < nf) {
                ins->op = XIR_MOV;
                ins->rep = XR_REP_I64;
                ins->args[0] = field_vregs[fidx];
                ins->args[1] = XIR_NONE;
            }
        }

        /* Object identity is gone, so the guard and the write barrier
         * are both vacuous on the materialised scalar form. */
        if (ins->op == XIR_GUARD_SHAPE &&
            xir_ref_is_vreg(ins->args[0]) && ins->args[0] == aref) {
            ins->op = XIR_NOP;
            ins->dst = XIR_NONE;
            ins->args[0] = XIR_NONE;
            ins->args[1] = XIR_NONE;
            ins->flags = 0;
        }
        if (ins->op == XIR_BARRIER_FWD &&
            xir_ref_is_vreg(ins->args[0]) && ins->args[0] == aref) {
            ins->op = XIR_NOP;
            ins->dst = XIR_NONE;
            ins->args[0] = XIR_NONE;
            ins->args[1] = XIR_NONE;
            ins->flags = 0;
        }
    }

    /* Retire the original ALLOC; DCE will remove the NOP later. */
    XirIns *alloc_ins = &blk->ins[a->ins_idx];
    alloc_ins->op = XIR_NOP;
    alloc_ins->dst = XIR_NONE;
    alloc_ins->args[0] = XIR_NONE;
    alloc_ins->args[1] = XIR_NONE;
    alloc_ins->flags = 0;

    xr_free(field_vregs);
    return true;
}

void xir_pass_escape_analysis(XirFunc *func) {
    if (!func) return;

    EaAlloc *allocs = NULL;
    uint32_t nalloc = ea_collect_allocs(func, &allocs);
    if (nalloc == 0) { xr_free(allocs); return; }

    /* Single function-wide escape-set computation.  Cost is
     *   O(ninstr * args) + O(call_arg_pool) + O(nphi)
     * which beats the previous O(nalloc * nblk^2) Pass 2d handily. */
    ea_mark_escapes(func, allocs, nalloc);

    /* Block-local scalar replacement for survivors.  Cross-block
     * Mem2Reg is the obvious next step; it requires dom-frontier phi
     * insertion and will land in a follow-up once we have the general
     * helpers for it. */
    for (uint32_t j = 0; j < nalloc; j++) {
        if (!allocs[j].escapes)
            (void)ea_scalar_replace(func, &allocs[j]);
    }

    xr_free(allocs);
}

/*
 * Allocation Sinking: move XIR_ALLOC from a dominating block into the
 * specific branch where it's actually used. If all uses of an allocation
 * are in blocks dominated by one successor of a BR, the ALLOC is sunk
 * to that successor's entry, avoiding allocation on the other path.
 *
 * This runs before escape_analysis so that sunk allocations can still
 * be scalar-replaced within their new (smaller) scope.
 */
void xir_pass_alloc_sink(XirFunc *func) {
    if (!func || func->nblk < 3 || func->nvreg == 0) return;

    uint32_t nblk = func->nblk;

    const XirDomTree *dt = xir_func_get_domtree(func);
    if (!dt) return;

    for (uint32_t bi = 0; bi < nblk; bi++) {
        XirBlock *blk = func->blocks[bi];
        if (!blk || blk->nins == 0) continue;

        for (uint32_t ii = 0; ii < blk->nins; ii++) {
            XirIns *ins = &blk->ins[ii];
            if (ins->op != XIR_ALLOC) continue;
            if (!xir_ref_is_vreg(ins->dst)) continue;
            XirRef aref = ins->dst;

            // Collect all use blocks for this allocation
            uint32_t use_blocks[64];
            uint32_t nuse = 0;
            bool overflow = false;

            for (uint32_t ub = 0; ub < nblk && !overflow; ub++) {
                XirBlock *ublk = func->blocks[ub];
                if (!ublk) continue;
                bool used_here = false;
                for (uint32_t k = 0; k < ublk->nins && !used_here; k++) {
                    if ((xir_ref_is_vreg(ublk->ins[k].args[0]) && ublk->ins[k].args[0] == aref) ||
                        (xir_ref_is_vreg(ublk->ins[k].args[1]) && ublk->ins[k].args[1] == aref))
                        used_here = true;
                }
                // Check phi args
                for (XirPhi *p = ublk->phis; p && !used_here; p = p->next) {
                    for (uint32_t a = 0; a < p->narg; a++) {
                        if (xir_ref_is_vreg(p->args[a]) && p->args[a] == aref) {
                            used_here = true; break;
                        }
                    }
                }
                // Check terminator
                if (!used_here && xir_ref_is_vreg(ublk->jmp.arg) && ublk->jmp.arg == aref)
                    used_here = true;

                if (used_here) {
                    if (nuse < 64) use_blocks[nuse++] = ub;
                    else overflow = true;
                }
            }
            if (overflow || nuse == 0) continue;

            // All uses in the same block as the ALLOC: no sinking needed
            bool all_same = true;
            for (uint32_t u = 0; u < nuse; u++)
                if (use_blocks[u] != bi) { all_same = false; break; }
            if (all_same) continue;

            // Check if the ALLOC block has a BR with two successors
            if (blk->jmp.type != XIR_JMP_BR || !blk->s1 || !blk->s2) continue;
            uint32_t s1_id = blk->s1->id;
            uint32_t s2_id = blk->s2->id;

            // Check if all uses are dominated by exactly one successor
            bool all_in_s1 = true, all_in_s2 = true;
            for (uint32_t u = 0; u < nuse; u++) {
                uint32_t ub = use_blocks[u];
                if (ub == bi) { all_in_s1 = false; all_in_s2 = false; break; }
                if (!xir_dom_covers(dt, s1_id, ub)) all_in_s1 = false;
                if (!xir_dom_covers(dt, s2_id, ub)) all_in_s2 = false;
            }

            XirBlock *sink_target = NULL;
            if (all_in_s1 && !all_in_s2) sink_target = blk->s1;
            else if (all_in_s2 && !all_in_s1) sink_target = blk->s2;
            if (!sink_target) continue;

            // Sink: move ALLOC to the beginning of sink_target
            if (sink_target->nins < sink_target->ins_cap) {
                // Shift existing instructions right by 1
                memmove(&sink_target->ins[1], &sink_target->ins[0],
                        sink_target->nins * sizeof(XirIns));
                sink_target->ins[0] = *ins;
                sink_target->nins++;
                // NOP the original
                ins->op = XIR_NOP;
                ins->dst = XIR_NONE;
                ins->args[0] = XIR_NONE;
                ins->args[1] = XIR_NONE;
                ins->flags = 0;
            }
        }
    }
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
static void *extract_const_ptr_from_vreg(XirFunc *func, XirRef ref) {
    if (xir_ref_is_const(ref)) {
        uint32_t ci = XIR_REF_INDEX(ref);
        if (ci < func->nconst)
            return func->consts[ci].val.ptr;
    }
    if (!xir_ref_is_vreg(ref)) return NULL;
    uint32_t vi = XIR_REF_INDEX(ref);
    if (vi >= func->nvreg) return NULL;
    XirIns *def = func->vregs[vi].def;
    if (!def || def->op != XIR_CONST_I64) return NULL;
    if (xir_ref_is_const(def->args[0])) {
        uint32_t ci = XIR_REF_INDEX(def->args[0]);
        if (ci < func->nconst)
            return func->consts[ci].val.ptr;
    }
    return NULL;
}

// Extract an integer constant from a ref (direct const or CONST_I64 vreg)
static bool extract_const_i64(XirFunc *func, XirRef ref, int64_t *out) {
    if (xir_ref_is_const(ref)) {
        uint32_t ci = XIR_REF_INDEX(ref);
        if (ci < func->nconst) { *out = func->consts[ci].val.i64; return true; }
    }
    if (!xir_ref_is_vreg(ref)) return false;
    uint32_t vi = XIR_REF_INDEX(ref);
    if (vi >= func->nvreg) return false;
    XirIns *def = func->vregs[vi].def;
    if (!def || def->op != XIR_CONST_I64) return false;
    if (xir_ref_is_const(def->args[0])) {
        uint32_t ci = XIR_REF_INDEX(def->args[0]);
        if (ci < func->nconst) { *out = func->consts[ci].val.i64; return true; }
    }
    return false;
}

void xir_pass_auto_inline(XirFunc *func, XrProto *caller_proto) {
    if (!func || !caller_proto) return;

    int inlined_count = 0;
    int inlined_size = 0;           // total bytecodes inlined (caller budget)
    const int max_inline = 12;      // max non-tiny inlinings (up for recursive chains)
    const int max_inlined_size = 350; // stop if caller grows too much

    // Per-callee inline history: limits recursive/mutual-recursive depth
    // and prevents excessive code bloat from repeated inlining of the same callee.
    #define MAX_INLINE_CALLEES 16
    const int max_callee_depth = 3;  // max times a single callee can be inlined
    struct { XrProto *proto; int count; } callee_hist[MAX_INLINE_CALLEES];
    int n_callee_hist = 0;
    int rounds_since_sub_pipeline = 0;

    // Iterate blocks; inlining modifies block array so restart after each inline
    bool did_inline = true;
    while (did_inline && inlined_count < max_inline && inlined_size < max_inlined_size) {
        did_inline = false;

        for (uint32_t bi = 0; bi < func->nblk && !did_inline; bi++) {
            XirBlock *blk = func->blocks[bi];
            if (!blk) continue;

            for (uint32_t ii = 0; ii < blk->nins && !did_inline; ii++) {
                XirIns *ins = &blk->ins[ii];
                bool is_call_known = (ins->op == XIR_CALL_KNOWN);
                bool is_call_reg   = (ins->op == XIR_CALL_KNOWN_REG);
                if (!is_call_known && !is_call_reg) continue;

                // --- Extract callee proto and arguments ---
                XrProto *callee = NULL;
                int nargs = 0;
                XirRef call_args[8];
                for (int k = 0; k < 8; k++) call_args[k] = XIR_NONE;

                // Read call args from the call_arg_pool via dst vreg
                uint32_t dst_vi = xir_ref_is_vreg(ins->dst) ? XIR_REF_INDEX(ins->dst) : func->nvreg;
                XirVReg *dst_vr = (dst_vi < func->nvreg) ? &func->vregs[dst_vi] : NULL;

                if (is_call_known) {
                    // CALL_KNOWN: args[0]=const_ptr(proto), args[1]=const(nargs)
                    callee = (XrProto *)extract_const_ptr_from_vreg(func, ins->args[0]);
                    if (!callee) continue;

                    int64_t n = 0;
                    if (!extract_const_i64(func, ins->args[1], &n)) continue;
                    nargs = (int)n;
                    if (nargs < 0 || nargs > 7) continue;

                    // Extract arguments from call_arg_pool
                    // Pool layout: [closure, param0, param1, ...] → params start at index 1
                    if (!dst_vr || dst_vr->call_nargs < (uint16_t)(1 + nargs)) continue;
                    for (int ai = 0; ai < nargs; ai++)
                        call_args[ai] = func->call_arg_pool[dst_vr->call_arg_start + 1 + ai];
                } else {
                    // CALL_KNOWN_REG: args[0]=param0, args[1]=param1
                    // Proto is in call_arg_pool slot 0.
                    // For CALL_KNOWN_REG, proto was stored by builder into jit_ctx->call_proto.
                    // Extract from pool slot 0 if available.
                    if (dst_vr && dst_vr->call_nargs > 0) {
                        XirRef proto_ref = func->call_arg_pool[dst_vr->call_arg_start];
                        callee = (XrProto *)extract_const_ptr_from_vreg(func, proto_ref);
                    }
                    if (!callee) continue;

                    // Arguments directly from ins->args
                    if (!xir_ref_is_none(ins->args[0])) {
                        call_args[0] = ins->args[0];
                        nargs = 1;
                        if (!xir_ref_is_none(ins->args[1])) {
                            call_args[1] = ins->args[1];
                            nargs = 2;
                        }
                    }
                }

                // --- Heuristic checks ---
                int code_count = (int)PROTO_CODE_COUNT(callee);

                // Per-callee depth check: limits recursive and repeated inlining.
                // Each callee can be inlined at most max_callee_depth times.
                int callee_depth = 0;
                for (int k = 0; k < n_callee_hist; k++) {
                    if (callee_hist[k].proto == callee) {
                        callee_depth = callee_hist[k].count;
                        break;
                    }
                }
                if (callee_depth >= max_callee_depth) continue;

                // Loop-depth bonus: calls inside loops are more profitable
                int depth_mult = (xir_block_loop_depth(func, blk->id) > 0) ? 2 : 1;

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
                if (!should_inline) continue;

                // Has upvalues: skip (upvalue capture is complex)
                if (PROTO_UPVAL_COUNT(callee) > 0) continue;

                // Has nested protos (closures): skip
                if (PROTO_PROTO_COUNT(callee) > 0) continue;

                // Generator or has defaults: skip
                if (callee->entry_type != 0) continue;

                // --- Build callee XIR ---
                XirFunc *callee_func = xir_build_from_proto(callee);
                if (!callee_func) continue;
                int max_blk = is_tiny ? 8 : 32;
                if (callee_func->nblk == 0 || callee_func->nblk > (uint32_t)max_blk) {
                    xir_func_destroy(callee_func);
                    continue;
                }

                // --- Inline ---
                XirRef result = xir_inline_function(func, blk, ii, callee_func,
                                                     call_args, (uint32_t)nargs);

                // If inline succeeded, replace uses of call result
                if (!xir_ref_is_none(result) && !xir_ref_is_none(ins->dst)) {
                    XirRef old_dst = ins->dst;
                    for (uint32_t rb = 0; rb < func->nblk; rb++) {
                        XirBlock *rblk = func->blocks[rb];
                        if (!rblk) continue;
                        for (uint32_t ri = 0; ri < rblk->nins; ri++) {
                            XirIns *rins = &rblk->ins[ri];
                            if (xir_ref_is_vreg(rins->args[0]) && rins->args[0] == old_dst)
                                rins->args[0] = result;
                            if (xir_ref_is_vreg(rins->args[1]) && rins->args[1] == old_dst)
                                rins->args[1] = result;
                        }
                        if (xir_ref_is_vreg(rblk->jmp.arg) && rblk->jmp.arg == old_dst)
                            rblk->jmp.arg = result;
                        for (XirPhi *phi = rblk->phis; phi; phi = phi->next) {
                            for (uint32_t pi = 0; pi < phi->narg; pi++) {
                                if (phi->args[pi] == old_dst)
                                    phi->args[pi] = result;
                            }
                        }
                    }
                }

                // Propagate callee return type to inlined result vreg
                if (!xir_ref_is_none(result) && xir_ref_is_vreg(result)) {
                    uint32_t rvi = XIR_REF_INDEX(result);
                    if (rvi < func->nvreg && !func->vregs[rvi].xrtype &&
                        callee->return_type_info)
                        func->vregs[rvi].xrtype = callee->return_type_info;
                }

                xir_func_destroy(callee_func);

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
                if (!is_tiny) inlined_count++;
                did_inline = true;
                rounds_since_sub_pipeline++;
            }
        }

        // Lightweight sub-pipeline every 3 inlines to expose new call sites.
        // TypeProp + Specialize can turn polymorphic calls into CALL_KNOWN,
        // enabling cascading inlining including recursive cases.
        if (did_inline && rounds_since_sub_pipeline >= 3) {
            xir_pass_type_prop(func);
            xir_pass_specialize(func);
            xir_pass_dce(func);
            rounds_since_sub_pipeline = 0;
        }
    }

    #undef MAX_INLINE_CALLEES
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
void xir_pass_elim_guards(XirFunc *func) {
    if (!func || func->nblk == 0) return;

    #define GUARD_TABLE_SIZE 32
    #define GUARD_TABLE_MASK (GUARD_TABLE_SIZE - 1)
    struct { XirRef arg0; XirRef arg1; uint16_t op; bool valid; }
        seen[GUARD_TABLE_SIZE];

    for (uint32_t bi = 0; bi < func->nblk; bi++) {
        XirBlock *blk = func->blocks[bi];
        memset(seen, 0, sizeof(seen));

        for (uint32_t i = 0; i < blk->nins; i++) {
            XirIns *ins = &blk->ins[i];

            if (ins->op != XIR_GUARD_TAG && ins->op != XIR_GUARD_SHAPE &&
                ins->op != XIR_GUARD_NONNULL && ins->op != XIR_GUARD_CLASS &&
                ins->op != XIR_GUARD_KLASS)
                continue;

            /* Type-aware guard elimination: if type_prop already proved
             * the vreg has the guarded tag, the guard is redundant.
             * This is the cross-block counterpart of per-block dedup. */
            if (ins->op == XIR_GUARD_TAG && xir_ref_is_vreg(ins->args[0])) {
                XirType gct = xir_ref_ctype(func, ins->args[0]);
                uint8_t gvtag = type_kind_to_vtag(gct.kind);
                if (gvtag != VTAG_TAGGED) {
                    // Check if guarded tag matches known tag
                    if (xir_ref_is_const(ins->args[1])) {
                        uint32_t ci = XIR_REF_INDEX(ins->args[1]);
                        if (ci < func->nconst) {
                            uint8_t expected = (uint8_t)func->consts[ci].val.raw;
                            if (vtag_to_value_tag(gvtag) == expected) {
                                ins->op = XIR_NOP;
                                ins->dst = XIR_NONE;
                                ins->args[0] = XIR_NONE;
                                ins->args[1] = XIR_NONE;
                                ins->flags = 0;
                                continue;
                            }
                        }
                    }
                }
            }

            // GUARD_NONNULL: eliminate if vreg is known PTR or fresh alloc
            if (ins->op == XIR_GUARD_NONNULL && xir_ref_is_vreg(ins->args[0])) {
                uint32_t vi = XIR_REF_INDEX(ins->args[0]);
                XirType gct = xir_ref_ctype(func, ins->args[0]);
                if (vi < func->nvreg &&
                    (xir_type_is_ptr(gct.kind) ||
                     func->vregs[vi].is_fresh_alloc)) {
                    ins->op = XIR_NOP;
                    ins->dst = XIR_NONE;
                    ins->args[0] = XIR_NONE;
                    ins->args[1] = XIR_NONE;
                    ins->flags = 0;
                    continue;
                }
            }

            // GUARD_CLASS: eliminate if heap_type already known and matches
            if (ins->op == XIR_GUARD_CLASS && xir_ref_is_vreg(ins->args[0])) {
                uint32_t vi = XIR_REF_INDEX(ins->args[0]);
                if (vi < func->nvreg && func->vregs[vi].heap_type != 0) {
                    if (xir_ref_is_const(ins->args[1])) {
                        uint32_t ci = XIR_REF_INDEX(ins->args[1]);
                        if (ci < func->nconst) {
                            uint16_t expected_ht = (uint16_t)func->consts[ci].val.raw;
                            if (func->vregs[vi].heap_type == expected_ht) {
                                ins->op = XIR_NOP;
                                ins->dst = XIR_NONE;
                                ins->args[0] = XIR_NONE;
                                ins->args[1] = XIR_NONE;
                                ins->flags = 0;
                                continue;
                            }
                        }
                    }
                }
            }

            // Per-block dedup: hash the guard signature
            uint32_t h = ((uint32_t)ins->op * 31 +
                          (uint32_t)ins->args[0] * 17 +
                          (uint32_t)ins->args[1]) & GUARD_TABLE_MASK;

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
                if (seen[slot].op == ins->op &&
                    seen[slot].arg0 == ins->args[0] &&
                    seen[slot].arg1 == ins->args[1]) {
                    found = true;
                    break;
                }
            }

            if (found) {
                // Duplicate guard — convert to NOP
                ins->op = XIR_NOP;
                ins->dst = XIR_NONE;
                ins->args[0] = XIR_NONE;
                ins->args[1] = XIR_NONE;
                ins->flags = 0;
            }
        }
    }
    #undef GUARD_TABLE_SIZE
    #undef GUARD_TABLE_MASK
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
static bool propjnz_replace_in_block(XirBlock *blk, XirRef old_ref, XirRef new_ref) {
    bool changed = false;

    for (uint32_t i = 0; i < blk->nins; i++) {
        XirIns *ins = &blk->ins[i];
        for (int a = 0; a < 2; a++) {
            if (xir_ref_is_vreg(ins->args[a]) && ins->args[a] == old_ref) {
                ins->args[a] = new_ref;
                changed = true;
            }
        }
    }

    /* Don't replace in phi args — they correspond to values from predecessors,
     * not values computed within this block */

    if (xir_ref_is_vreg(blk->jmp.arg) && blk->jmp.arg == old_ref) {
        blk->jmp.arg = new_ref;
        changed = true;
    }

    return changed;
}

// Helper: check if a vreg is defined by a CONST_I64 and return its value
static bool propjnz_is_const(XirFunc *func, XirRef ref, int64_t *val) {
    if (!xir_ref_is_vreg(ref)) return false;
    uint32_t idx = XIR_REF_INDEX(ref);
    if (idx >= func->nvreg) return false;
    XirIns *def = func->vregs[idx].def;
    if (!def || def->op != XIR_CONST_I64) return false;
    if (!xir_ref_is_const(def->args[0])) return false;
    uint32_t ci = XIR_REF_INDEX(def->args[0]);
    if (ci >= func->nconst) return false;
    *val = func->consts[ci].val.i64;
    return true;
}

void xir_pass_propjnz(XirFunc *func) {
    if (!func || func->nblk < 2) return;

    for (uint32_t bi = 0; bi < func->nblk; bi++) {
        XirBlock *blk = func->blocks[bi];
        if (blk->jmp.type != XIR_JMP_BR) continue;

        XirRef cond = blk->jmp.arg;
        if (!xir_ref_is_vreg(cond)) continue;

        XirBlock *s_true  = blk->s1; // taken when cond != 0
        XirBlock *s_false = blk->s2; // taken when cond == 0
        if (!s_true || !s_false) continue;

        /* Rule 1: In false successor (single-pred), cond is 0.
         * Replace uses of cond with a zero constant. */
        if (s_false->npred == 1) {
            XirRef zero_vreg = XIR_NONE;
            // Search for an existing vreg defined as CONST_I64(0)
            for (uint32_t v = 0; v < func->nvreg; v++) {
                int64_t val;
                XirRef vref = XIR_REF(XIR_REF_VREG, v);
                if (propjnz_is_const(func, vref, &val) && val == 0) {
                    zero_vreg = vref;
                    break;
                }
            }
            if (!xir_ref_is_none(zero_vreg)) {
                propjnz_replace_in_block(s_false, cond, zero_vreg);
            }
        }

        // Find the defining instruction of cond
        uint32_t cond_idx = XIR_REF_INDEX(cond);
        if (cond_idx >= func->nvreg) continue;
        XirIns *def = func->vregs[cond_idx].def;
        if (!def) continue;

        // Rule 2: cond = EQ(a, K) → true successor: a == K
        if (def->op == XIR_EQ && s_true->npred == 1) {
            XirRef a = def->args[0];
            XirRef b = def->args[1];
            int64_t val;
            if (xir_ref_is_vreg(a) && propjnz_is_const(func, b, &val)) {
                propjnz_replace_in_block(s_true, a, b);
            } else if (xir_ref_is_vreg(b) && propjnz_is_const(func, a, &val)) {
                propjnz_replace_in_block(s_true, b, a);
            }
        }

        // Rule 3: cond = NE(a, K) → false successor: a == K
        if (def->op == XIR_NE && s_false->npred == 1) {
            XirRef a = def->args[0];
            XirRef b = def->args[1];
            int64_t val;
            if (xir_ref_is_vreg(a) && propjnz_is_const(func, b, &val)) {
                propjnz_replace_in_block(s_false, a, b);
            } else if (xir_ref_is_vreg(b) && propjnz_is_const(func, a, &val)) {
                propjnz_replace_in_block(s_false, b, a);
            }
        }
    }
}

/* ========== Global Code Motion (GCM) ========== */

/*
 * Click's GCM algorithm adapted for XIR:
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
#define GCM_MAX_BLOCKS XIR_GCM_MAX_BLOCKS
#define GCM_MAX_VREGS  XIR_GCM_MAX_VREGS

static bool gcm_is_pinned(uint16_t op) {
    return !xir_op_is_pure(op);
}

// Compute dominator depth for each block
static void gcm_fill_depth(uint32_t *idom, uint32_t *depth, uint32_t nblk) {
    for (uint32_t i = 0; i < nblk; i++) depth[i] = 0;
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
    if (a == GCM_NOBID) return b;
    if (b == GCM_NOBID) return a;
    while (a != b) {
        while (depth[a] > depth[b]) { if (a == 0) return 0; a = idom[a]; }
        while (depth[b] > depth[a]) { if (b == 0) return 0; b = idom[b]; }
        if (a == b) break;
        if (a == 0 || b == 0) return 0;
        a = idom[a]; b = idom[b];
    }
    return a;
}

// Early schedule: find earliest possible block for vreg v
static uint32_t gcm_early(XirFunc *func, uint32_t *idom, uint32_t *depth,
                           uint32_t *early, uint32_t *def_blk, uint32_t v) {
    if (early[v] != GCM_NOBID) return early[v];

    XirIns *def = func->vregs[v].def;
    if (!def) {
        // Phi-defined or no def: stays in defining block
        early[v] = def_blk[v];
        return early[v];
    }

    // Find the deepest dominator where both operands are available
    early[v] = 0; // mark visiting (entry block)
    uint32_t best = 0;
    for (int a = 0; a < 2; a++) {
        if (!xir_ref_is_vreg(def->args[a])) continue;
        uint32_t av = XIR_REF_INDEX(def->args[a]);
        if (av >= func->nvreg) continue;
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

void xir_pass_gcm(XirFunc *func) {
    if (!func || func->nblk < 2 || func->nvreg == 0) return;
    if (func->nblk > GCM_MAX_BLOCKS || func->nvreg > GCM_MAX_VREGS) return;

    uint32_t nblk = func->nblk;
    uint32_t nv = func->nvreg;

    // Step 0: Build infrastructure (domtree is cached in func)
    const XirDomTree *dt = xir_func_get_domtree(func);
    if (!dt) return;
    uint32_t *idom = dt->idom;

    uint32_t *depth = (uint32_t *)xr_malloc(nblk * sizeof(uint32_t));
    uint32_t *early = (uint32_t *)xr_malloc(nv * sizeof(uint32_t));
    uint32_t *best = (uint32_t *)xr_malloc(nv * sizeof(uint32_t));
    uint32_t *def_blk = (uint32_t *)xr_calloc(nv, sizeof(uint32_t));
    if (!depth || !early || !best || !def_blk) {
        xr_free(depth); xr_free(early); xr_free(best); xr_free(def_blk);
        return;
    }
    gcm_fill_depth(idom, depth, nblk);

    // Build def_blk map for each vreg
    for (uint32_t v = 0; v < nv; v++) {
        early[v] = GCM_NOBID;
        best[v] = GCM_NOBID;
    }
    for (uint32_t bi = 0; bi < nblk; bi++) {
        XirBlock *blk = func->blocks[bi];
        for (XirPhi *p = blk->phis; p; p = p->next) {
            if (xir_ref_is_vreg(p->dst)) {
                uint32_t v = XIR_REF_INDEX(p->dst);
                if (v < nv) def_blk[v] = bi;
            }
        }
        for (uint32_t i = 0; i < blk->nins; i++) {
            XirIns *ins = &blk->ins[i];
            if (xir_ref_is_vreg(ins->dst)) {
                uint32_t v = XIR_REF_INDEX(ins->dst);
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
    uint32_t *late = best; // reuse array
    for (uint32_t v = 0; v < nv; v++) late[v] = GCM_NOBID;

    for (uint32_t bi = 0; bi < nblk; bi++) {
        XirBlock *blk = func->blocks[bi];
        // Instruction arg uses
        for (uint32_t i = 0; i < blk->nins; i++) {
            XirIns *ins = &blk->ins[i];
            for (int a = 0; a < 2; a++) {
                if (!xir_ref_is_vreg(ins->args[a])) continue;
                uint32_t v = XIR_REF_INDEX(ins->args[a]);
                if (v < nv) late[v] = gcm_lca(idom, depth, late[v], bi);
            }
        }
        // Phi arg uses: use belongs to the predecessor block
        for (XirPhi *p = blk->phis; p; p = p->next) {
            for (uint32_t a = 0; a < p->narg && a < blk->npred; a++) {
                if (!xir_ref_is_vreg(p->args[a])) continue;
                uint32_t v = XIR_REF_INDEX(p->args[a]);
                if (v < nv && blk->preds[a])
                    late[v] = gcm_lca(idom, depth, late[v], blk->preds[a]->id);
            }
        }
        // Terminator arg use
        if (xir_ref_is_vreg(blk->jmp.arg)) {
            uint32_t v = XIR_REF_INDEX(blk->jmp.arg);
            if (v < nv) late[v] = gcm_lca(idom, depth, late[v], bi);
        }
    }

    // Step 3: Best placement — walk early→late, pick min loop_depth
    uint32_t moved = 0;
    for (uint32_t v = 0; v < nv; v++) {
        XirIns *def = func->vregs[v].def;
        if (!def || gcm_is_pinned(def->op)) continue;
        uint32_t e = early[v];
        uint32_t l = late[v];
        if (e == GCM_NOBID || l == GCM_NOBID) continue;
        if (e == def_blk[v] && l == def_blk[v]) continue;

        // Verify early dominates late
        if (!xir_dom_covers(dt, e, l)) continue;

        // Walk from late up to early, find best loop depth
        uint32_t best_bid = l;
        int best_depth = (int)xir_block_loop_depth(func, func->blocks[l]->id);
        uint32_t cur = l;
        uint32_t limit = nblk;
        while (cur != e && limit-- > 0) {
            cur = idom[cur];
            if (cur >= nblk) break;
            int ld = (int)xir_block_loop_depth(func, func->blocks[cur]->id);
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
        xr_free(depth); xr_free(early); xr_free(best); xr_free(def_blk);
        return;
    }

    /* Step 4: Move instructions to their best blocks.
     * NOP the instruction in the original block and append to the target. */
    for (uint32_t v = 0; v < nv; v++) {
        if (best[v] == GCM_NOBID) continue;
        XirIns *def = func->vregs[v].def;
        if (!def) continue;

        XirBlock *target = func->blocks[best[v]];
        // Append instruction to target block
        if (target->nins < target->ins_cap) {
            target->ins[target->nins] = *def;
            func->vregs[v].def = &target->ins[target->nins];
            def_blk[v] = best[v];
            target->nins++;
            // NOP the original
            def->op = XIR_NOP;
            def->dst = XIR_NONE;
            def->args[0] = XIR_NONE;
            def->args[1] = XIR_NONE;
        }
    }

    /* Step 5: Block-internal DFS scheduling to fix def-before-use.
     * After moving instructions, a use might precede its definition
     * within the same block. Topological sort via DFS fixes this. */
    {
        // Build vreg→instruction-index-in-block map for scheduling
        XirIns *tmp_ins = (XirIns *)xr_malloc(256 * sizeof(XirIns));
        uint8_t *scheduled = (uint8_t *)xr_calloc(nv, sizeof(uint8_t));
        if (tmp_ins && scheduled) {
            for (uint32_t bi = 0; bi < nblk; bi++) {
                XirBlock *blk = func->blocks[bi];
                if (blk->nins <= 1) continue;

                uint32_t out = 0;
                uint32_t cap = 256;
                memset(scheduled, 0, nv);

                /* Simple topological sort: for each instruction in order,
                 * ensure its operand defs (if in same block) are emitted first */
                for (uint32_t i = 0; i < blk->nins && out < cap; i++) {
                    XirIns *ins = &blk->ins[i];
                    if (ins->op == XIR_NOP) continue;

                    // Check if operands need to be scheduled first
                    for (int a = 0; a < 2; a++) {
                        if (!xir_ref_is_vreg(ins->args[a])) continue;
                        uint32_t av = XIR_REF_INDEX(ins->args[a]);
                        if (av >= nv || scheduled[av] || def_blk[av] != bi) continue;
                        // Find and emit the operand's definition
                        for (uint32_t j = i + 1; j < blk->nins; j++) {
                            if (xir_ref_is_vreg(blk->ins[j].dst) &&
                                XIR_REF_INDEX(blk->ins[j].dst) == av &&
                                blk->ins[j].op != XIR_NOP) {
                                if (out < cap) tmp_ins[out++] = blk->ins[j];
                                blk->ins[j].op = XIR_NOP;
                                scheduled[av] = 1;
                                break;
                            }
                        }
                    }

                    if (ins->op != XIR_NOP && out < cap) {
                        tmp_ins[out++] = *ins;
                        if (xir_ref_is_vreg(ins->dst)) {
                            uint32_t dv = XIR_REF_INDEX(ins->dst);
                            if (dv < nv) scheduled[dv] = 1;
                        }
                    }
                }

                // Copy back
                if (out > 0 && out <= blk->ins_cap) {
                    memcpy(blk->ins, tmp_ins, out * sizeof(XirIns));
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
}

/* ========== Type Propagation Pass ========== */

/*
 * Map xr_tag (XrValueTag) to canonical XrType*.
 * Used to narrow vreg type after GUARD_TAG succeeds.
 */
static XrType *xr_tag_to_xrtype(uint8_t tag) {
    switch (tag) {
    case 1: case 2:                     // TRUE, FALSE
        return xr_type_new_bool();
    case 3: case 4: case 5: case 6:     // I8..I64
    case 7: case 8: case 9: case 10:    // U8..U64
        return xr_type_new_int();
    case 11: case 12:                   // F32, F64
        return xr_type_new_float();
    case 13:                            // PTR
        return NULL;                    // PTR needs heap_type for precision
    default:
        return NULL;
    }
}

/*
 * Map XrType* to vtag (XrVRegTag) for compile-time type annotation.
 */
static uint8_t xrtype_to_vtag(XrType *t) {
    if (!t) return VTAG_TAGGED;
    switch (t->kind) {
    case XR_KIND_INT:   return VTAG_I64;
    case XR_KIND_FLOAT: return VTAG_F64;
    case XR_KIND_BOOL:  return VTAG_BOOL;
    case XR_KIND_STRING:
    case XR_KIND_ARRAY:
    case XR_KIND_MAP:
    case XR_KIND_CLASS:
    case XR_KIND_FUNCTION:
        return VTAG_PTR;
    case XR_KIND_UNION:
        return value_tag_to_vtag(xr_type_to_xr_tag(t));
    default:
        return VTAG_TAGGED;
    }
}

/* Refine vreg vtag: only upgrade TAGGED → concrete.
 * Syncs ins->ctype on the defining instruction. */
static void refine_vreg_vtag(XirFunc *func, uint32_t vi, uint8_t vtag) {
    if (vi >= func->nvreg) return;
    XirIns *def = func->vregs[vi].def;
    // Only upgrade UNKNOWN → concrete (guard via ctype / rep)
    if (def && def->ctype.kind != XIR_TK_UNKNOWN) return;
    if (!def && func->vregs[vi].rep != XR_REP_TAGGED) return;
    if (def) def->ctype.kind = vtag_to_type_kind(vtag);
}

// Force-set vreg ctype unconditionally.
static void force_vreg_vtag(XirFunc *func, uint32_t vi, uint8_t vtag) {
    if (vi >= func->nvreg) return;
    XirIns *def = func->vregs[vi].def;
    if (def) def->ctype.kind = vtag_to_type_kind(vtag);
}

/* Set xrtype and vtag on a vreg. Allows refinement:
 * - TAGGED → any concrete tag (normal narrowing)
 * - PTR with unknown heap_type → PTR with known heap_type
 * - xrtype upgrade from NULL to non-NULL */
static void set_vreg_type(XirFunc *func, uint32_t vi, XrType *t, uint8_t vtag) {
    if (vi >= func->nvreg) return;
    if (!func->vregs[vi].xrtype)
        func->vregs[vi].xrtype = t;
    refine_vreg_vtag(func, vi, vtag);
}

/* Set heap_type on a vreg (for ALLOC, GUARD_CLASS, etc.).
 * Only sets if current heap_type is 0 (unknown). */
static void set_vreg_heap_type(XirFunc *func, uint32_t vi, uint16_t ht) {
    if (vi >= func->nvreg) return;
    if (func->vregs[vi].heap_type == 0) {
        func->vregs[vi].heap_type = ht;
        // Sync heap_cid on defining instruction
        XirIns *def = func->vregs[vi].def;
        if (def && def->ctype.heap_cid == 0)
            def->ctype.heap_cid = ht;
    }
}

/* Compute the meet (join) of two vtags in the type lattice.
 *   same → same
 *   I64 + F64 → NUMERIC (runtime check needed)
 *   PTR + PTR → PTR (different heap types, but both pointers)
 *   concrete + TAGGED → TAGGED
 *   anything else → TAGGED */
static uint8_t meet_vtag(uint8_t a, uint8_t b) {
    if (a == b) return a;
    // int + float → numeric union
    if ((a == VTAG_I64 && b == VTAG_F64) || (a == VTAG_F64 && b == VTAG_I64))
        return VTAG_NUMERIC;
    // numeric + int/float → numeric
    if ((a == VTAG_NUMERIC && (b == VTAG_I64 || b == VTAG_F64)) ||
        (b == VTAG_NUMERIC && (a == VTAG_I64 || a == VTAG_F64)))
        return VTAG_NUMERIC;
    // ptr + ptr → ptr (different heap types widen)
    if (a == VTAG_PTR && b == VTAG_PTR)
        return VTAG_PTR;
    // everything else falls to tagged
    return VTAG_TAGGED;
}

/*
 * xir_pass_type_prop: forward pass that propagates type information.
 *
 * Covers:
 *   - Constants (I64, F64, PTR)
 *   - Guards (GUARD_TAG, GUARD_NONNULL, GUARD_CLASS)
 *   - Arithmetic (int, float, mixed-type RT_*)
 *   - Comparisons (always bool)
 *   - Calls (CALL_KNOWN, CALL_KNOWN_REG, CALL_DIRECT, CALL_SELF_DIRECT)
 *   - Memory loads (LOAD_CORO, LOAD_FIELD, typed loads)
 *   - Runtime helpers (RT_ARRAY_NEW/LEN, RT_MAP_NEW, RT_ISNULL, etc.)
 *   - BOX/UNBOX, type conversions, MOV, NEG, SELECT
 *   - PHI meet with type lattice widening
 *   - ALLOC with heap_type extraction
 */
/* One forward scan of the type lattice.  Wrapped by xir_pass_type_prop
 * below which iterates until no vreg type changes. */
static void type_prop_scan_once(XirFunc *func) {
    XrType *t_int   = xr_type_new_int();
    XrType *t_float = xr_type_new_float();
    XrType *t_bool  = xr_type_new_bool();

    for (uint32_t bi = 0; bi < func->nblk; bi++) {
        XirBlock *blk = func->blocks[bi];
        if (!blk) continue;

        /* PHI type meet with lattice widening:
         *   all same → propagate
         *   int + float → NUMERIC
         *   PTR + PTR (different heap_type) → PTR
         *   any TAGGED input → give up */
        for (XirPhi *phi = blk->phis; phi; phi = phi->next) {
            if (!xir_ref_is_vreg(phi->dst)) continue;
            uint32_t dvi = XIR_REF_INDEX(phi->dst);
            if (dvi >= func->nvreg) continue;
            if (xir_ref_ctype(func, phi->dst).kind != XIR_TK_UNKNOWN) continue;
            if (phi->narg == 0) continue;

            uint8_t result_vtag = VTAG_TAGGED;
            bool valid = true;

            for (uint16_t a = 0; a < phi->narg; a++) {
                if (!xir_ref_is_vreg(phi->args[a])) { valid = false; break; }
                uint32_t ai = XIR_REF_INDEX(phi->args[a]);
                if (ai >= func->nvreg) { valid = false; break; }
                uint8_t atag = type_kind_to_vtag(xir_ref_ctype(func, phi->args[a]).kind);
                if (atag == VTAG_TAGGED) { valid = false; break; }

                if (a == 0) {
                    result_vtag = atag;
                } else {
                    result_vtag = meet_vtag(result_vtag, atag);
                    if (result_vtag == VTAG_TAGGED) { valid = false; break; }
                }
            }

            if (valid && result_vtag != VTAG_TAGGED) {
                force_vreg_vtag(func, dvi, result_vtag);
                // Set xrtype for uniform cases
                if (!func->vregs[dvi].xrtype) {
                    switch (result_vtag) {
                    case VTAG_I64:  func->vregs[dvi].xrtype = t_int; break;
                    case VTAG_F64:  func->vregs[dvi].xrtype = t_float; break;
                    case VTAG_BOOL: func->vregs[dvi].xrtype = t_bool; break;
                    default: break;
                    }
                }
            }
        }

        for (uint32_t i = 0; i < blk->nins; i++) {
            XirIns *ins = &blk->ins[i];

            switch (ins->op) {

            // ---- Constants ----
            case XIR_CONST_I64:
                if (xir_ref_is_vreg(ins->dst)) {
                    uint32_t vi = XIR_REF_INDEX(ins->dst);
                    set_vreg_type(func, vi, t_int, VTAG_I64);
                }
                break;

            case XIR_CONST_F64:
                if (xir_ref_is_vreg(ins->dst)) {
                    uint32_t vi = XIR_REF_INDEX(ins->dst);
                    set_vreg_type(func, vi, t_float, VTAG_F64);
                }
                break;

            case XIR_CONST_PTR:
                if (xir_ref_is_vreg(ins->dst)) {
                    uint32_t vi = XIR_REF_INDEX(ins->dst);
                    refine_vreg_vtag(func, vi, VTAG_PTR);
                }
                break;

            // ---- Guards: narrow guarded vreg ----
            case XIR_GUARD_TAG: {
                if (!xir_ref_is_vreg(ins->args[0])) break;
                if (!xir_ref_is_const(ins->args[1])) break;
                uint32_t ci = XIR_REF_INDEX(ins->args[1]);
                if (ci >= func->nconst) break;
                uint8_t expected_tag = (uint8_t)func->consts[ci].val.raw;
                XrType *narrowed = xr_tag_to_xrtype(expected_tag);
                uint32_t vi = XIR_REF_INDEX(ins->args[0]);
                if (vi < func->nvreg) {
                    if (narrowed && !func->vregs[vi].xrtype)
                        func->vregs[vi].xrtype = narrowed;
                    refine_vreg_vtag(func, vi, value_tag_to_vtag(expected_tag));
                }
                break;
            }

            case XIR_GUARD_NONNULL: {
                if (!xir_ref_is_vreg(ins->args[0])) break;
                uint32_t vi = XIR_REF_INDEX(ins->args[0]);
                refine_vreg_vtag(func, vi, VTAG_PTR);
                break;
            }

            case XIR_GUARD_CLASS: {
                if (!xir_ref_is_vreg(ins->args[0])) break;
                uint32_t vi = XIR_REF_INDEX(ins->args[0]);
                if (vi < func->nvreg) {
                    refine_vreg_vtag(func, vi, VTAG_PTR);
                    // Extract heap_type from guard constant
                    if (xir_ref_is_const(ins->args[1])) {
                        uint32_t ci = XIR_REF_INDEX(ins->args[1]);
                        if (ci < func->nconst) {
                            uint16_t ht = (uint16_t)func->consts[ci].val.raw;
                            set_vreg_heap_type(func, vi, ht);
                        }
                    }
                }
                break;
            }

            case XIR_GUARD_KLASS: {
                // After klass guard, receiver is known to be PTR (instance)
                if (!xir_ref_is_vreg(ins->args[0])) break;
                uint32_t vi = XIR_REF_INDEX(ins->args[0]);
                if (vi < func->nvreg) {
                    refine_vreg_vtag(func, vi, VTAG_PTR);
                    set_vreg_heap_type(func, vi, XR_TINSTANCE);
                }
                break;
            }

            // ---- Integer arithmetic ----
            case XIR_ADD: case XIR_SUB: case XIR_MUL:
            case XIR_AND: case XIR_OR:  case XIR_XOR:
            case XIR_SHL: case XIR_SHR: {
                if (!xir_ref_is_vreg(ins->dst)) break;
                uint32_t dvi = XIR_REF_INDEX(ins->dst);
                if (dvi >= func->nvreg || func->vregs[dvi].xrtype) break;
                XrType *at = NULL, *bt = NULL;
                if (xir_ref_is_vreg(ins->args[0])) {
                    uint32_t ai = XIR_REF_INDEX(ins->args[0]);
                    if (ai < func->nvreg) at = func->vregs[ai].xrtype;
                }
                if (xir_ref_is_vreg(ins->args[1])) {
                    uint32_t bi2 = XIR_REF_INDEX(ins->args[1]);
                    if (bi2 < func->nvreg) bt = func->vregs[bi2].xrtype;
                }
                if (at && bt) {
                    if (at == t_int && bt == t_int)
                        set_vreg_type(func, dvi, t_int, VTAG_I64);
                    else if (at->kind == XR_KIND_FLOAT || bt->kind == XR_KIND_FLOAT)
                        set_vreg_type(func, dvi, t_float, VTAG_F64);
                } else if (at == t_int || bt == t_int) {
                    set_vreg_type(func, dvi, t_int, VTAG_I64);
                }
                break;
            }

            case XIR_NOT: {
                if (!xir_ref_is_vreg(ins->dst)) break;
                uint32_t dvi = XIR_REF_INDEX(ins->dst);
                if (dvi >= func->nvreg || func->vregs[dvi].xrtype) break;
                // Bitwise NOT preserves int type
                set_vreg_type(func, dvi, t_int, VTAG_I64);
                break;
            }

            case XIR_DIV: case XIR_MOD: {
                if (!xir_ref_is_vreg(ins->dst)) break;
                uint32_t dvi = XIR_REF_INDEX(ins->dst);
                if (dvi >= func->nvreg || func->vregs[dvi].xrtype) break;
                if (func->vregs[dvi].rep == XR_REP_F64)
                    set_vreg_type(func, dvi, t_float, VTAG_F64);
                else if (func->vregs[dvi].rep == XR_REP_I64)
                    set_vreg_type(func, dvi, t_int, VTAG_I64);
                break;
            }

            // ---- Comparisons: always bool ----
            case XIR_EQ: case XIR_NE: case XIR_LT:
            case XIR_LE: case XIR_GT: case XIR_GE:
            case XIR_FEQ: case XIR_FNE: case XIR_FLT: case XIR_FLE: {
                if (!xir_ref_is_vreg(ins->dst)) break;
                uint32_t dvi = XIR_REF_INDEX(ins->dst);
                if (dvi >= func->nvreg) break;
                set_vreg_type(func, dvi, t_bool, VTAG_BOOL);
                break;
            }

            // ---- Mixed-type runtime comparisons: always bool ----
            case XIR_RT_LT: case XIR_RT_LE: case XIR_RT_EQ: {
                if (!xir_ref_is_vreg(ins->dst)) break;
                uint32_t dvi = XIR_REF_INDEX(ins->dst);
                if (dvi >= func->nvreg) break;
                set_vreg_type(func, dvi, t_bool, VTAG_BOOL);
                break;
            }

            // ---- Mixed-type runtime arithmetic: numeric result ----
            case XIR_RT_ADD: case XIR_RT_SUB: case XIR_RT_MUL:
            case XIR_RT_DIV: case XIR_RT_MOD: {
                if (!xir_ref_is_vreg(ins->dst)) break;
                uint32_t dvi = XIR_REF_INDEX(ins->dst);
                if (dvi >= func->nvreg || func->vregs[dvi].xrtype) break;
                // Infer from operand types if possible
                uint8_t atag = type_kind_to_vtag(xir_ref_ctype(func, ins->args[0]).kind);
                uint8_t btag = type_kind_to_vtag(xir_ref_ctype(func, ins->args[1]).kind);
                if (atag == VTAG_I64 && btag == VTAG_I64 &&
                    ins->op != XIR_RT_DIV) {
                    set_vreg_type(func, dvi, t_int, VTAG_I64);
                } else if (atag == VTAG_F64 || btag == VTAG_F64) {
                    set_vreg_type(func, dvi, t_float, VTAG_F64);
                } else if ((atag == VTAG_I64 || atag == VTAG_F64 ||
                            atag == VTAG_NUMERIC) &&
                           (btag == VTAG_I64 || btag == VTAG_F64 ||
                            btag == VTAG_NUMERIC)) {
                    // Both numeric but mixed → numeric union
                    refine_vreg_vtag(func, dvi, VTAG_NUMERIC);
                }
                break;
            }

            case XIR_RT_UNM: {
                if (!xir_ref_is_vreg(ins->dst)) break;
                uint32_t dvi = XIR_REF_INDEX(ins->dst);
                if (dvi >= func->nvreg || func->vregs[dvi].xrtype) break;
                // Unary minus preserves operand type
                if (xir_ref_is_vreg(ins->args[0])) {
                    uint32_t ai = XIR_REF_INDEX(ins->args[0]);
                    if (ai < func->nvreg && func->vregs[ai].xrtype)
                        set_vreg_type(func, dvi, func->vregs[ai].xrtype,
                                      type_kind_to_vtag(xir_ref_ctype(func, ins->args[0]).kind));
                }
                break;
            }

            // ---- Runtime array/collection helpers ----
            case XIR_RT_ARRAY_NEW:
            case XIR_RT_MAP_NEW: {
                if (!xir_ref_is_vreg(ins->dst)) break;
                uint32_t dvi = XIR_REF_INDEX(ins->dst);
                if (dvi >= func->nvreg) break;
                refine_vreg_vtag(func, dvi, VTAG_PTR);
                set_vreg_heap_type(func, dvi,
                    ins->op == XIR_RT_ARRAY_NEW ? XR_TARRAY : XR_TMAP);
                break;
            }

            case XIR_RT_ARRAY_LEN: {
                if (!xir_ref_is_vreg(ins->dst)) break;
                uint32_t dvi = XIR_REF_INDEX(ins->dst);
                set_vreg_type(func, dvi, t_int, VTAG_I64);
                break;
            }

            case XIR_RT_ISNULL: {
                if (!xir_ref_is_vreg(ins->dst)) break;
                uint32_t dvi = XIR_REF_INDEX(ins->dst);
                set_vreg_type(func, dvi, t_bool, VTAG_BOOL);
                break;
            }

            // ---- Calls with known return type ----
            case XIR_CALL_KNOWN:
            case XIR_CALL_KNOWN_REG: {
                if (!xir_ref_is_vreg(ins->dst)) break;
                uint32_t dvi = XIR_REF_INDEX(ins->dst);
                if (dvi >= func->nvreg) break;
                if (!xir_ref_is_const(ins->args[0])) break;
                uint32_t ci = XIR_REF_INDEX(ins->args[0]);
                if (ci >= func->nconst) break;
                XrProto *callee = (XrProto *)func->consts[ci].val.ptr;
                if (callee && callee->return_type_info) {
                    XrType *rt = callee->return_type_info;
                    if (!func->vregs[dvi].xrtype)
                        func->vregs[dvi].xrtype = rt;
                    uint8_t inferred = xrtype_to_vtag(rt);
                    if (inferred != VTAG_TAGGED)
                        refine_vreg_vtag(func, dvi, inferred);
                }
                break;
            }

            case XIR_CALL_SELF_DIRECT:
            case XIR_CALL_DIRECT: {
                // callee_proto stored on dst vreg by builder
                if (!xir_ref_is_vreg(ins->dst)) break;
                uint32_t dvi = XIR_REF_INDEX(ins->dst);
                if (dvi >= func->nvreg) break;
                XrProto *cp = func->vregs[dvi].callee_proto;
                if (cp && cp->return_type_info) {
                    XrType *rt = cp->return_type_info;
                    if (!func->vregs[dvi].xrtype)
                        func->vregs[dvi].xrtype = rt;
                    uint8_t inferred = xrtype_to_vtag(rt);
                    if (inferred != VTAG_TAGGED)
                        refine_vreg_vtag(func, dvi, inferred);
                }
                break;
            }

            case XIR_CALL_C: {
                // C calls: infer from dst rep when possible
                if (!xir_ref_is_vreg(ins->dst)) break;
                uint32_t dvi = XIR_REF_INDEX(ins->dst);
                if (dvi >= func->nvreg) break;
                // If codegen assigned a specific rep, trust it
                if (ins->ctype.kind == XIR_TK_UNKNOWN) {
                    uint8_t rv = VTAG_TAGGED;
                    switch (func->vregs[dvi].rep) {
                    case XR_REP_I64: rv = VTAG_I64; break;
                    case XR_REP_F64: rv = VTAG_F64; break;
                    case XR_REP_PTR: rv = VTAG_PTR; break;
                    default: break;
                    }
                    if (rv != VTAG_TAGGED) refine_vreg_vtag(func, dvi, rv);
                }
                break;
            }

            // ---- Unary: NEG, FNEG ----
            case XIR_NEG: {
                if (!xir_ref_is_vreg(ins->dst)) break;
                uint32_t dvi = XIR_REF_INDEX(ins->dst);
                if (dvi >= func->nvreg || func->vregs[dvi].xrtype) break;
                if (xir_ref_is_vreg(ins->args[0])) {
                    uint32_t ai = XIR_REF_INDEX(ins->args[0]);
                    if (ai < func->nvreg && func->vregs[ai].xrtype)
                        set_vreg_type(func, dvi, func->vregs[ai].xrtype,
                                      type_kind_to_vtag(xir_ref_ctype(func, ins->args[0]).kind));
                }
                break;
            }

            // ---- MOV: propagate type + tag ----
            case XIR_MOV: {
                if (!xir_ref_is_vreg(ins->dst)) break;
                uint32_t dvi = XIR_REF_INDEX(ins->dst);
                if (dvi >= func->nvreg) break;
                if (xir_ref_is_vreg(ins->args[0])) {
                    uint32_t ai = XIR_REF_INDEX(ins->args[0]);
                    if (ai < func->nvreg) {
                        if (!func->vregs[dvi].xrtype && func->vregs[ai].xrtype)
                            func->vregs[dvi].xrtype = func->vregs[ai].xrtype;
                        uint8_t ak = type_kind_to_vtag(xir_ref_ctype(func, ins->args[0]).kind);
                        if (ak != VTAG_TAGGED)
                            refine_vreg_vtag(func, dvi, ak);
                        // Propagate heap_type
                        if (func->vregs[dvi].heap_type == 0 &&
                            func->vregs[ai].heap_type != 0)
                            func->vregs[dvi].heap_type = func->vregs[ai].heap_type;
                    }
                }
                break;
            }

            // ---- SELECT: meet of true/false branches ----
            case XIR_SELECT: {
                if (!xir_ref_is_vreg(ins->dst)) break;
                uint32_t dvi = XIR_REF_INDEX(ins->dst);
                if (dvi >= func->nvreg) break;
                if (ins->ctype.kind != XIR_TK_UNKNOWN) break;
                uint8_t atag = type_kind_to_vtag(xir_ref_ctype(func, ins->args[0]).kind);
                uint8_t btag = type_kind_to_vtag(xir_ref_ctype(func, ins->args[1]).kind);
                if (atag != VTAG_TAGGED && btag != VTAG_TAGGED) {
                    uint8_t mt = meet_vtag(atag, btag);
                    if (mt != VTAG_TAGGED)
                        refine_vreg_vtag(func, dvi, mt);
                }
                break;
            }

            // ---- UNBOX: known result type ----
            case XIR_UNBOX_I64: {
                if (!xir_ref_is_vreg(ins->dst)) break;
                uint32_t dvi = XIR_REF_INDEX(ins->dst);
                set_vreg_type(func, dvi, t_int, VTAG_I64);
                break;
            }
            case XIR_UNBOX_F64: {
                if (!xir_ref_is_vreg(ins->dst)) break;
                uint32_t dvi = XIR_REF_INDEX(ins->dst);
                set_vreg_type(func, dvi, t_float, VTAG_F64);
                break;
            }

            // ---- BOX: propagate source tag ----
            case XIR_BOX_I64: {
                if (!xir_ref_is_vreg(ins->dst)) break;
                uint32_t dvi = XIR_REF_INDEX(ins->dst);
                refine_vreg_vtag(func, dvi, VTAG_I64);
                break;
            }
            case XIR_BOX_F64: {
                if (!xir_ref_is_vreg(ins->dst)) break;
                uint32_t dvi = XIR_REF_INDEX(ins->dst);
                refine_vreg_vtag(func, dvi, VTAG_F64);
                break;
            }

            // ---- Float arithmetic: always float ----
            case XIR_FADD: case XIR_FSUB: case XIR_FMUL: case XIR_FDIV:
            case XIR_FNEG: {
                if (!xir_ref_is_vreg(ins->dst)) break;
                uint32_t dvi = XIR_REF_INDEX(ins->dst);
                set_vreg_type(func, dvi, t_float, VTAG_F64);
                break;
            }

            // ---- Type conversions ----
            case XIR_I2F: {
                if (!xir_ref_is_vreg(ins->dst)) break;
                uint32_t dvi = XIR_REF_INDEX(ins->dst);
                set_vreg_type(func, dvi, t_float, VTAG_F64);
                break;
            }
            case XIR_F2I: {
                if (!xir_ref_is_vreg(ins->dst)) break;
                uint32_t dvi = XIR_REF_INDEX(ins->dst);
                set_vreg_type(func, dvi, t_int, VTAG_I64);
                break;
            }

            // ---- ALLOC: PTR + extract heap_type from packed constant ----
            case XIR_ALLOC: {
                if (!xir_ref_is_vreg(ins->dst)) break;
                uint32_t dvi = XIR_REF_INDEX(ins->dst);
                if (dvi >= func->nvreg) break;
                if (ins->ctype.kind == XIR_TK_UNKNOWN) {
                    ins->ctype.kind = XIR_TK_PTR;
                }
                // Extract gc_type from args[0] packed constant:
                // packed = (gc_extra << 8) | gc_type
                if (xir_ref_is_const(ins->args[0])) {
                    uint32_t ci = XIR_REF_INDEX(ins->args[0]);
                    if (ci < func->nconst) {
                        uint16_t gc_type = (uint16_t)(func->consts[ci].val.i64 & 0xFF);
                        set_vreg_heap_type(func, dvi, gc_type);
                    }
                }
                func->vregs[dvi].is_fresh_alloc = true;
                break;
            }

            // ---- Memory loads: deterministic result types ----
            case XIR_LOAD_CORO:
            case XIR_LOAD_CORO_BYTE:
            case XIR_LOAD32S:
            case XIR_LOAD8Z: case XIR_LOAD8S:
            case XIR_LOAD16Z: case XIR_LOAD16S:
            case XIR_LOAD32Z:
            case XIR_TAG_LOAD: {
                if (!xir_ref_is_vreg(ins->dst)) break;
                uint32_t dvi = XIR_REF_INDEX(ins->dst);
                set_vreg_type(func, dvi, t_int, VTAG_I64);
                break;
            }

            case XIR_LOAD_F32: {
                if (!xir_ref_is_vreg(ins->dst)) break;
                uint32_t dvi = XIR_REF_INDEX(ins->dst);
                set_vreg_type(func, dvi, t_float, VTAG_F64);
                break;
            }

            // ---- LOAD_FIELD: infer from vreg rep ----
            case XIR_LOAD_FIELD: {
                if (!xir_ref_is_vreg(ins->dst)) break;
                uint32_t dvi = XIR_REF_INDEX(ins->dst);
                if (dvi >= func->nvreg) break;
                if (ins->ctype.kind == XIR_TK_UNKNOWN) {
                    uint8_t rv = VTAG_TAGGED;
                    switch (func->vregs[dvi].rep) {
                    case XR_REP_I64: rv = VTAG_I64; break;
                    case XR_REP_F64: rv = VTAG_F64; break;
                    case XR_REP_PTR: rv = VTAG_PTR; break;
                    default: break;
                    }
                    if (rv != VTAG_TAGGED) {
                        ins->ctype.kind = vtag_to_type_kind(rv);
                    }
                }
                break;
            }

            // ---- RT_PRINT: void, skip ----
            case XIR_RT_PRINT:
                break;

            // ---- CATCH: exception object is always PTR ----
            case XIR_CATCH: {
                if (!xir_ref_is_vreg(ins->dst)) break;
                uint32_t dvi = XIR_REF_INDEX(ins->dst);
                if (dvi >= func->nvreg) break;
                if (ins->ctype.kind == XIR_TK_UNKNOWN)
                    ins->ctype.kind = XIR_TK_PTR;
                break;
            }

            default:
                break;
            }
        }
    }
}

/*
 * FNV-1a hash of every vreg's (xrtype, ctype.kind, heap_type).  The
 * hash collapses to the same value iff no refinement was made during
 * the last scan, which lets xir_pass_type_prop detect convergence in
 * O(nvreg) without threading return values through hundreds of
 * setter call sites.
 */
static uint64_t type_prop_checksum(XirFunc *func) {
    uint64_t h = 1469598103934665603ull;
    for (uint32_t v = 0; v < func->nvreg; v++) {
        uintptr_t t = (uintptr_t)func->vregs[v].xrtype;
        uint8_t   k = 0;
        uint16_t  ht = func->vregs[v].heap_type;
        if (func->vregs[v].def)
            k = (uint8_t)func->vregs[v].def->ctype.kind;
        h ^= (uint64_t)t; h *= 1099511628211ull;
        h ^= k;            h *= 1099511628211ull;
        h ^= ht;           h *= 1099511628211ull;
    }
    return h;
}

/* Worklist-style driver: iterate scan_once until the type state
 * stops changing.  In practice two or three rounds suffice even on
 * deeply chained IR; the cap guards against pathological input. */
#define TYPE_PROP_MAX_ROUNDS 6

void xir_pass_type_prop(XirFunc *func) {
    if (!func || func->nblk == 0) return;
    for (int round = 0; round < TYPE_PROP_MAX_ROUNDS; round++) {
        uint64_t before = type_prop_checksum(func);
        type_prop_scan_once(func);
        if (type_prop_checksum(func) == before) break;
    }
}

/* ========== Type-Driven Specialization ========== */

/*
 * xir_pass_specialize: lower polymorphic RT_* instructions to monomorphic
 * native ops when type_prop has proven operand types.
 *
 * This runs after type_prop and converts:
 *   RT_LT/LE/EQ(I64,I64) → LT/LE/EQ   (native int compare)
 *   RT_LT/LE/EQ(F64,F64) → FLT/FLE/FEQ (native float compare)
 *   RT_ADD/SUB/MUL/DIV(F64,F64) → FADD/FSUB/FMUL/FDIV
 *
 * WHY THIS DESIGN:
 *   The builder emits RT_* when operand types are unknown at build time.
 *   After type_prop (which may run 3-5 times through the pipeline), operand
 *   types may be proven. Lowering to native ops enables subsequent passes
 *   (CSE, LICM, GVN, if-conversion) to optimize these operations, which
 *   they cannot do with opaque RT_* instructions.
 */
void xir_pass_specialize(XirFunc *func) {
    if (!func || func->nblk == 0) return;

    for (uint32_t bi = 0; bi < func->nblk; bi++) {
        XirBlock *blk = func->blocks[bi];
        if (!blk) continue;

        for (uint32_t i = 0; i < blk->nins; i++) {
            XirIns *ins = &blk->ins[i];

            // Get operand vtags for binary ops
            uint8_t ta = VTAG_TAGGED, tb = VTAG_TAGGED;
            uint8_t ra = XR_REP_TAGGED, rb = XR_REP_TAGGED;
            if (xir_ref_is_vreg(ins->args[0])) {
                uint32_t ai = XIR_REF_INDEX(ins->args[0]);
                if (ai < func->nvreg) {
                    ta = type_kind_to_vtag(xir_ref_ctype(func, ins->args[0]).kind);
                    ra = func->vregs[ai].rep;
                }
            }
            if (xir_ref_is_vreg(ins->args[1])) {
                uint32_t bi2 = XIR_REF_INDEX(ins->args[1]);
                if (bi2 < func->nvreg) {
                    tb = type_kind_to_vtag(xir_ref_ctype(func, ins->args[1]).kind);
                    rb = func->vregs[bi2].rep;
                }
            }

            switch (ins->op) {

            // ---- RT comparisons → native comparisons ----
            case XIR_RT_LT: case XIR_RT_LE: case XIR_RT_EQ: {
                if (ta == VTAG_I64 && tb == VTAG_I64 &&
                    ra == XR_REP_I64 && rb == XR_REP_I64) {
                    // Both proven I64: lower to native int compare
                    if (ins->op == XIR_RT_LT)      ins->op = XIR_LT;
                    else if (ins->op == XIR_RT_LE)  ins->op = XIR_LE;
                    else                            ins->op = XIR_EQ;
                    ins->rep = XR_REP_I64;
                    ins->flags &= ~XIR_FLAG_SIDE_EFFECT;
                } else if (ta == VTAG_F64 && tb == VTAG_F64 &&
                           ra == XR_REP_F64 && rb == XR_REP_F64) {
                    // Both proven F64: lower to native float compare
                    if (ins->op == XIR_RT_LT)      ins->op = XIR_FLT;
                    else if (ins->op == XIR_RT_LE)  ins->op = XIR_FLE;
                    else                            ins->op = XIR_FEQ;
                    ins->rep = XR_REP_I64;
                    ins->flags &= ~XIR_FLAG_SIDE_EFFECT;
                }
                break;
            }

            // ---- RT arithmetic (both F64) → native float ops ----
            case XIR_RT_ADD: case XIR_RT_SUB: case XIR_RT_MUL:
            case XIR_RT_DIV: {
                if (ra == XR_REP_F64 && rb == XR_REP_F64) {
                    switch (ins->op) {
                    case XIR_RT_ADD: ins->op = XIR_FADD; break;
                    case XIR_RT_SUB: ins->op = XIR_FSUB; break;
                    case XIR_RT_MUL: ins->op = XIR_FMUL; break;
                    case XIR_RT_DIV: ins->op = XIR_FDIV; break;
                    default: break;
                    }
                    ins->rep = XR_REP_F64;
                    ins->flags &= ~XIR_FLAG_SIDE_EFFECT;
                }
                break;
            }

            default:
                break;
            }
        }
    }
}

/* ========== Write Barrier Elimination ========== */

/*
 * Per-block elimination of redundant write barriers.
 *
 * Two rules (inspired by Dart VM's EliminateWriteBarriers):
 *
 * Rule 1 — New allocation:
 *   If the parent object of BARRIER_FWD was allocated (XIR_ALLOC) in the
 *   same block, and no GC-triggering instruction (SAFEPOINT, CALL_C,
 *   CALL_KNOWN) occurred between the allocation and the barrier, then
 *   the object is guaranteed to be in young space → no barrier needed.
 *
 * Rule 2 — Duplicate barrier:
 *   If the same parent object already has a BARRIER_FWD earlier in the
 *   block (with no intervening GC trigger), subsequent barriers on the
 *   same parent are redundant — the object is already in the remembered
 *   set from the first barrier.
 *
 * Both rule sets are invalidated by any instruction that can trigger GC,
 * since GC may promote young objects to old space or flush the store buffer.
 */
static bool can_trigger_gc(uint16_t op) {
    switch (op) {
    case XIR_SAFEPOINT:
    case XIR_CALL_C:
    case XIR_CALL_KNOWN:
    case XIR_ALLOC:
        return true;
    default:
        return false;
    }
}

void xir_pass_elim_write_barriers(XirFunc *func) {
    if (!func) return;

    /* Track up to 32 "known-safe" parent vregs per block.
     * A vreg is safe if it was freshly allocated (Rule 1) or
     * already had a barrier (Rule 2) since the last GC point. */
    #define WBE_MAX_SAFE 32

    for (uint32_t bi = 0; bi < func->nblk; bi++) {
        XirBlock *blk = func->blocks[bi];
        if (!blk) continue;

        XirRef safe_parents[WBE_MAX_SAFE];
        uint32_t nsafe = 0;

        for (uint32_t i = 0; i < blk->nins; i++) {
            XirIns *ins = &blk->ins[i];

            // GC trigger: invalidate all tracked safe parents
            if (can_trigger_gc(ins->op)) {
                // But XIR_ALLOC also defines a new young object
                if (ins->op == XIR_ALLOC && xir_ref_is_vreg(ins->dst)) {
                    nsafe = 0;
                    if (nsafe < WBE_MAX_SAFE)
                        safe_parents[nsafe++] = ins->dst;
                } else {
                    nsafe = 0;
                }
                continue;
            }

            if (ins->op != XIR_BARRIER_FWD) continue;

            // args[0] = parent object
            XirRef parent = ins->args[0];
            if (!xir_ref_is_vreg(parent)) continue;

            // Check if parent is in safe set
            bool is_safe = false;
            for (uint32_t s = 0; s < nsafe; s++) {
                if (safe_parents[s] == parent) {
                    is_safe = true;
                    break;
                }
            }

            if (is_safe) {
                // Eliminate this barrier
                ins->op = XIR_NOP;
                ins->dst = XIR_NONE;
                ins->args[0] = XIR_NONE;
                ins->args[1] = XIR_NONE;
                ins->flags = 0;
            } else {
                /* Not safe yet — add parent to safe set (Rule 2:
                 * after one barrier, object is in remembered set) */
                if (nsafe < WBE_MAX_SAFE)
                    safe_parents[nsafe++] = parent;
            }
        }
    }

    #undef WBE_MAX_SAFE
}

/* ========== Range Analysis ========== */

/*
 * Range analysis for integer values, inspired by Dart's RangeAnalysis.
 *
 * Goals:
 *   1. Track loop induction variables: i=0; i<N; i++ → range [0, N-1]
 *   2. Eliminate redundant GUARD_BOUNDS where index is provably in [0, length)
 *   3. Propagate range constraints from GUARD_BOUNDS to dominated uses
 *   4. Symbolic bounds: track i < arr.length to eliminate GUARD_BOUNDS(i, len)
 *
 * Algorithm:
 *   Phase 1: Collect all integer definitions and GUARD_BOUNDS
 *   Phase 2: Detect simple induction variables (phi with init + stride)
 *   Phase 3: Forward propagate ranges (iterative to fixed point, max 4 rounds)
 *   Phase 4: Eliminate redundant GUARD_BOUNDS (constant + symbolic)
 */

#define RA_MAX_VALUES XIR_RA_MAX_VALUES
#define RA_MAX_ROUNDS XIR_RA_MAX_ROUNDS

typedef struct {
    int64_t lo;        // inclusive lower bound
    int64_t hi;        // inclusive upper bound (constant)
    XirRef  sym_bound; // symbolic strict upper bound: value < sym_bound
                       // XIR_NONE if no symbolic bound
    bool    known;     // true if range is valid
} XirRange;

// Try to get constant value from a vreg
static bool ra_get_const(XirFunc *func, XirRef ref, int64_t *out) {
    if (!xir_ref_is_vreg(ref)) return false;
    uint32_t vi = XIR_REF_INDEX(ref);
    if (vi >= func->nvreg) return false;
    XirIns *def = func->vregs[vi].def;
    if (!def || def->op != XIR_CONST_I64) return false;
    if (!xir_ref_is_const(def->args[0])) return false;
    uint32_t ci = XIR_REF_INDEX(def->args[0]);
    if (ci >= func->nconst) return false;
    *out = func->consts[ci].val.i64;
    return true;
}

/* Detect simple induction variable pattern:
 *   phi(init, update) where update = phi + stride
 * Returns true if detected, sets *init_val and *stride_val */
static bool ra_detect_induction(XirFunc *func, XirPhi *phi,
                                 int64_t *init_val, int64_t *stride_val) {
    if (!phi || phi->narg != 2) return false;
    if (!xir_ref_is_vreg(phi->dst)) return false;

    // Try both orderings: (init, update) and (update, init)
    for (int order = 0; order < 2; order++) {
        XirRef init_ref = phi->args[order];
        XirRef update_ref = phi->args[1 - order];

        // init must be a constant
        int64_t init;
        if (!ra_get_const(func, init_ref, &init)) continue;

        // update must be phi +/- constant
        if (!xir_ref_is_vreg(update_ref)) continue;
        uint32_t ui = XIR_REF_INDEX(update_ref);
        if (ui >= func->nvreg) continue;
        XirIns *udef = func->vregs[ui].def;
        if (!udef) continue;

        int64_t stride;
        if (udef->op == XIR_ADD) {
            // update = a + b, one of which is phi, the other is const stride
            if (udef->args[0] == phi->dst) {
                if (!ra_get_const(func, udef->args[1], &stride)) continue;
            } else if (udef->args[1] == phi->dst) {
                if (!ra_get_const(func, udef->args[0], &stride)) continue;
            } else continue;
        } else if (udef->op == XIR_SUB) {
            // update = phi - const
            if (udef->args[0] != phi->dst) continue;
            if (!ra_get_const(func, udef->args[1], &stride)) continue;
            stride = -stride;
        } else continue;

        *init_val = init;
        *stride_val = stride;
        return true;
    }
    return false;
}

/* Find the loop bound for an induction variable by looking at BR conditions.
 * Pattern: BR(LT(iv, bound)) or BR(GE(iv, bound)) at a back-edge block.
 * Returns true if a constant or symbolic bound is found.
 * sym_ref receives the symbolic bound vreg (e.g., arr.length) when
 * the bound is not a constant. sym_ref is XIR_NONE for constant bounds. */
static bool ra_find_loop_bound(XirFunc *func, XirBlock *header, XirRef iv_ref,
                                int64_t *bound_val, XirRef *sym_ref) {
    *sym_ref = XIR_NONE;
    for (uint32_t pi = 0; pi < header->npred; pi++) {
        XirBlock *pred = header->preds[pi];
        if (!pred || pred->nins == 0) continue;
        if (pred->jmp.type != XIR_BR) continue;

        for (int32_t ii = (int32_t)pred->nins - 1; ii >= 0; ii--) {
            XirIns *ins = &pred->ins[ii];

            if (ins->op == XIR_LT || ins->op == XIR_LE) {
                if (ins->args[0] == iv_ref) {
                    int64_t bv;
                    if (ra_get_const(func, ins->args[1], &bv)) {
                        *bound_val = (ins->op == XIR_LT) ? bv - 1 : bv;
                        return true;
                    }
                    /* Non-constant bound: track as symbolic.
                     * iv < sym means iv in [lo, sym-1] symbolically. */
                    if (ins->op == XIR_LT && xir_ref_is_vreg(ins->args[1])) {
                        *sym_ref = ins->args[1];
                        *bound_val = INT64_MAX;
                        return true;
                    }
                }
            } else if (ins->op == XIR_GT || ins->op == XIR_GE) {
                if (ins->args[1] == iv_ref) {
                    int64_t bv;
                    if (ra_get_const(func, ins->args[0], &bv)) {
                        *bound_val = (ins->op == XIR_GT) ? bv - 1 : bv;
                        return true;
                    }
                }
            }
            if ((int32_t)pred->nins - 1 - ii > 3) break;
        }
    }
    return false;
}

void xir_pass_range_analysis(XirFunc *func) {
    if (!func || func->nvreg == 0 || func->nblk == 0) return;

    uint32_t nv = func->nvreg;
    if (nv > RA_MAX_VALUES) return; // bail on very large functions

    // Phase 1: Initialize range info for all vregs
    XirRange *ranges = (XirRange *)xr_calloc(nv, sizeof(XirRange));
    if (!ranges) return;

    // Set ranges for constants
    for (uint32_t bi = 0; bi < func->nblk; bi++) {
        XirBlock *blk = func->blocks[bi];
        if (!blk) continue;

        for (uint32_t i = 0; i < blk->nins; i++) {
            XirIns *ins = &blk->ins[i];
            if (!xir_ref_is_vreg(ins->dst)) continue;
            uint32_t dvi = XIR_REF_INDEX(ins->dst);
            if (dvi >= nv) continue;

            if (ins->op == XIR_CONST_I64) {
                int64_t val;
                if (ra_get_const(func, ins->dst, &val)) {
                    ranges[dvi].lo = val;
                    ranges[dvi].hi = val;
                    ranges[dvi].sym_bound = XIR_NONE;
                    ranges[dvi].known = true;
                }
            }
        }
    }

    // Phase 2: Detect induction variables and set their ranges
    for (uint32_t bi = 0; bi < func->nblk; bi++) {
        XirBlock *blk = func->blocks[bi];
        if (!blk) continue;

        for (XirPhi *phi = blk->phis; phi; phi = phi->next) {
            if (!xir_ref_is_vreg(phi->dst)) continue;
            uint32_t pvi = XIR_REF_INDEX(phi->dst);
            if (pvi >= nv) continue;

            int64_t init, stride;
            if (ra_detect_induction(func, phi, &init, &stride)) {
                int64_t bound;
                XirRef sym_ref;
                if (ra_find_loop_bound(func, blk, phi->dst, &bound, &sym_ref)) {
                    if (stride > 0 && (sym_ref != XIR_NONE || init <= bound)) {
                        ranges[pvi].lo = init;
                        ranges[pvi].hi = bound;
                        ranges[pvi].sym_bound = sym_ref;
                        ranges[pvi].known = true;
                    } else if (stride < 0 && sym_ref == XIR_NONE && bound <= init) {
                        ranges[pvi].lo = bound;
                        ranges[pvi].hi = init;
                        ranges[pvi].sym_bound = XIR_NONE;
                        ranges[pvi].known = true;
                    }
                } else if (stride > 0) {
                    ranges[pvi].lo = init;
                    ranges[pvi].hi = INT64_MAX;
                    ranges[pvi].sym_bound = XIR_NONE;
                    ranges[pvi].known = true;
                }
            }
        }
    }

    /* Phase 3: Forward propagate ranges through simple arithmetic.
     * Iterate up to RA_MAX_ROUNDS until no new ranges are discovered. */
    for (int round = 0; round < RA_MAX_ROUNDS; round++) {
        bool changed = false;

        for (uint32_t bi = 0; bi < func->nblk; bi++) {
            XirBlock *blk = func->blocks[bi];
            if (!blk) continue;

            for (uint32_t i = 0; i < blk->nins; i++) {
                XirIns *ins = &blk->ins[i];
                if (!xir_ref_is_vreg(ins->dst)) continue;
                uint32_t dvi = XIR_REF_INDEX(ins->dst);
                if (dvi >= nv || ranges[dvi].known) continue;

                if (ins->rep != XR_REP_I64) continue;

                XirRange r0 = {0}, r1 = {0};
                if (xir_ref_is_vreg(ins->args[0])) {
                    uint32_t ai = XIR_REF_INDEX(ins->args[0]);
                    if (ai < nv) r0 = ranges[ai];
                }
                if (xir_ref_is_vreg(ins->args[1])) {
                    uint32_t ai = XIR_REF_INDEX(ins->args[1]);
                    if (ai < nv) r1 = ranges[ai];
                }

                if (!r0.known || !r1.known) continue;

                switch (ins->op) {
                case XIR_ADD:
                    if (r0.lo >= 0 && r1.lo >= 0 &&
                        r0.hi <= INT64_MAX - r1.hi) {
                        ranges[dvi].lo = r0.lo + r1.lo;
                        ranges[dvi].hi = r0.hi + r1.hi;
                        ranges[dvi].sym_bound = XIR_NONE;
                        ranges[dvi].known = true;
                        changed = true;
                    }
                    break;
                case XIR_SUB:
                    if (r0.lo >= r1.hi && r0.lo >= 0) {
                        ranges[dvi].lo = r0.lo - r1.hi;
                        ranges[dvi].hi = r0.hi - r1.lo;
                        ranges[dvi].sym_bound = XIR_NONE;
                        ranges[dvi].known = true;
                        changed = true;
                    }
                    break;
                case XIR_AND:
                    if (r0.lo >= 0 && r1.lo >= 0) {
                        ranges[dvi].lo = 0;
                        ranges[dvi].hi = r0.hi < r1.hi ? r0.hi : r1.hi;
                        ranges[dvi].sym_bound = XIR_NONE;
                        ranges[dvi].known = true;
                        changed = true;
                    }
                    break;
                case XIR_MUL:
                    if (r0.lo >= 0 && r1.lo >= 0 &&
                        r0.hi <= 0x7FFFFFFF && r1.hi <= 0x7FFFFFFF) {
                        ranges[dvi].lo = r0.lo * r1.lo;
                        ranges[dvi].hi = r0.hi * r1.hi;
                        ranges[dvi].sym_bound = XIR_NONE;
                        ranges[dvi].known = true;
                        changed = true;
                    }
                    break;
                case XIR_MOD:
                    if (r0.lo >= 0 && r1.lo > 0) {
                        ranges[dvi].lo = 0;
                        int64_t max_mod = r1.hi - 1;
                        ranges[dvi].hi = (r0.hi < max_mod) ? r0.hi : max_mod;
                        ranges[dvi].sym_bound = XIR_NONE;
                        ranges[dvi].known = true;
                        changed = true;
                    }
                    break;
                case XIR_SHL:
                    if (r0.lo >= 0 && r1.lo >= 0 && r1.lo == r1.hi &&
                        r1.lo < 63 && r0.hi <= (INT64_MAX >> r1.lo)) {
                        ranges[dvi].lo = r0.lo << r1.lo;
                        ranges[dvi].hi = r0.hi << r1.lo;
                        ranges[dvi].sym_bound = XIR_NONE;
                        ranges[dvi].known = true;
                        changed = true;
                    }
                    break;
                case XIR_SHR:
                    if (r0.lo >= 0 && r1.lo >= 0 && r1.lo == r1.hi && r1.lo < 64) {
                        ranges[dvi].lo = 0;
                        ranges[dvi].hi = r0.hi >> r1.lo;
                        ranges[dvi].sym_bound = XIR_NONE;
                        ranges[dvi].known = true;
                        changed = true;
                    }
                    break;
                default:
                    break;
                }
            }
        }
        if (!changed) break;
    }

    // Phase 4: Eliminate redundant GUARD_BOUNDS + constraint propagation
    /* GUARD_BOUNDS: deopt if (unsigned)args[0] >= (unsigned)args[1]
     * Equivalent to: deopt if index < 0 || index >= length
     * Safe to eliminate if:
     *   (a) Constant: 0 <= index.lo && index.hi < length.lo
     *   (b) Symbolic: index.lo >= 0 && index.sym_bound == length_ref
     *       (iv < len proved by loop comparison, same vreg as GUARD_BOUNDS len)
     *
     * Constraint propagation: after a surviving GUARD_BOUNDS(idx, len),
     * we know idx is in [0, len-1]. This helps eliminate subsequent
     * GUARD_BOUNDS on the same index in the same block. */
    for (uint32_t bi = 0; bi < func->nblk; bi++) {
        XirBlock *blk = func->blocks[bi];
        if (!blk) continue;

        for (uint32_t i = 0; i < blk->nins; i++) {
            XirIns *ins = &blk->ins[i];
            if (ins->op != XIR_GUARD_BOUNDS) continue;

            XirRef idx_ref = ins->args[0];
            XirRef len_ref = ins->args[1];

            XirRange idx_range = {0};
            XirRange len_range = {0};

            if (xir_ref_is_vreg(idx_ref)) {
                uint32_t vi = XIR_REF_INDEX(idx_ref);
                if (vi < nv) idx_range = ranges[vi];
            }
            if (xir_ref_is_vreg(len_ref)) {
                uint32_t vi = XIR_REF_INDEX(len_ref);
                if (vi < nv) len_range = ranges[vi];
            }

            // (a) Constant elimination: index range provably within [0, length)
            if (idx_range.known && len_range.known &&
                idx_range.lo >= 0 && idx_range.hi < len_range.lo) {
                ins->op = XIR_NOP;
                ins->dst = XIR_NONE;
                ins->args[0] = XIR_NONE;
                ins->args[1] = XIR_NONE;
                ins->flags = 0;
                continue;
            }

            /* (b) Symbolic elimination: iv < sym_bound && sym_bound == len_ref
             * Pattern: for (i=0; i<arr.length; i++) arr[i] = ...
             * The IV i has sym_bound pointing to the arr.length vreg,
             * and GUARD_BOUNDS(i, arr.length) uses the same vreg. */
            if (idx_range.known && idx_range.lo >= 0 &&
                idx_range.sym_bound != XIR_NONE &&
                idx_range.sym_bound == len_ref) {
                ins->op = XIR_NOP;
                ins->dst = XIR_NONE;
                ins->args[0] = XIR_NONE;
                ins->args[1] = XIR_NONE;
                ins->flags = 0;
                continue;
            }

            /* Constraint propagation: after this guard passes,
             * index is in [0, len-1]. */
            if (xir_ref_is_vreg(idx_ref)) {
                uint32_t vi = XIR_REF_INDEX(idx_ref);
                if (vi < nv) {
                    if (!ranges[vi].known || ranges[vi].lo < 0)
                        ranges[vi].lo = 0;
                    ranges[vi].known = true;
                    if (len_range.known && len_range.lo > 0) {
                        int64_t max_idx = len_range.lo - 1;
                        if (ranges[vi].hi > max_idx)
                            ranges[vi].hi = max_idx;
                    }
                    // Also propagate symbolic bound from len_ref
                    if (ranges[vi].sym_bound == XIR_NONE)
                        ranges[vi].sym_bound = len_ref;
                }
            }
        }
    }

    xr_free(ranges);
}

#undef RA_MAX_VALUES
#undef RA_MAX_ROUNDS

/* ========== REDEFINE Insertion ========== */
/*
 * Insert XIR_REDEFINE after each GUARD instruction to create a new SSA
 * value with the narrowed type. Rewrites uses of the guarded vreg in the
 * same block (after the guard) and in all dominated blocks.
 *
 * Before: GUARD_TAG v0, TAG_INT
 *         ... uses v0 ...
 * After:  GUARD_TAG v0, TAG_INT
 *         v1 = REDEFINE v0   [ctype = INT]
 *         ... uses v1 ...
 *
 * This makes type narrowing flow-sensitive: only uses after the guard
 * see the narrowed type. Uses before the guard keep the original type.
 */
void xir_pass_insert_redefines(XirFunc *func) {
    if (!func || func->nblk == 0) return;

    const XirDomTree *dt = xir_func_get_domtree(func);

    for (uint32_t bi = 0; bi < func->nblk; bi++) {
        XirBlock *blk = func->blocks[bi];
        if (!blk) continue;

        // Process guards from END to START to avoid index shift issues
        for (int32_t ii = (int32_t)blk->nins - 1; ii >= 0; ii--) {
            XirIns *ins = &blk->ins[ii];

            // Determine guarded vreg and narrowed type
            XirRef guarded = XIR_NONE;
            XirType narrowed = XIR_TYPE_UNKNOWN;

            switch (ins->op) {
            case XIR_GUARD_TAG: {
                if (!xir_ref_is_vreg(ins->args[0])) continue;
                if (!xir_ref_is_const(ins->args[1])) continue;
                uint32_t ci = XIR_REF_INDEX(ins->args[1]);
                if (ci >= func->nconst) continue;
                uint8_t expected_tag = (uint8_t)func->consts[ci].val.raw;
                guarded = ins->args[0];
                narrowed.kind = vtag_to_type_kind(value_tag_to_vtag(expected_tag));
                break;
            }
            case XIR_GUARD_NONNULL:
                if (!xir_ref_is_vreg(ins->args[0])) continue;
                guarded = ins->args[0];
                narrowed.kind = XIR_TK_PTR;
                break;
            case XIR_GUARD_CLASS: {
                if (!xir_ref_is_vreg(ins->args[0])) continue;
                guarded = ins->args[0];
                narrowed.kind = XIR_TK_PTR;
                if (xir_ref_is_const(ins->args[1])) {
                    uint32_t ci = XIR_REF_INDEX(ins->args[1]);
                    if (ci < func->nconst)
                        narrowed.heap_cid = (uint16_t)func->consts[ci].val.raw;
                }
                break;
            }
            case XIR_GUARD_KLASS:
                if (!xir_ref_is_vreg(ins->args[0])) continue;
                guarded = ins->args[0];
                narrowed.kind = XIR_TK_PTR;
                break;
            default: continue;
            }

            if (xir_ref_is_none(guarded)) continue;
            uint32_t gvi = XIR_REF_INDEX(guarded);
            if (gvi >= func->nvreg) continue;

            // Skip if guarded vreg already has a concrete type (no narrowing needed)
            XirType gct = xir_ref_ctype(func, guarded);
            if (gct.kind != XIR_TK_UNKNOWN && narrowed.kind == gct.kind)
                continue;

            // Create new vreg for REDEFINE result
            uint8_t rep = func->vregs[gvi].rep;
            XirRef new_ref = xir_new_vreg(func, rep);
            uint32_t nvi = XIR_REF_INDEX(new_ref);
            if (nvi >= func->nvreg) continue;

            // Insert REDEFINE at position ii+1
            uint32_t insert_pos = (uint32_t)ii + 1;
            XirIns *redef = xir_block_insert_at(func, blk, insert_pos);
            if (!redef) continue;

            redef->op = XIR_REDEFINE;
            redef->rep = rep;
            redef->flags = 0;
            redef->ctype = narrowed;
            redef->dst = new_ref;
            redef->args[0] = guarded;
            redef->args[1] = XIR_NONE;

            // Link new vreg to defining instruction
            func->vregs[nvi].def = redef;
            func->vregs[nvi].heap_type = narrowed.heap_cid;

            // Rewrite uses of guarded vreg in same block AFTER the REDEFINE
            for (uint32_t j = insert_pos + 1; j < blk->nins; j++) {
                XirIns *use = &blk->ins[j];
                for (int a = 0; a < 2; a++) {
                    if (use->args[a] == guarded)
                        use->args[a] = new_ref;
                }
                // Don't rewrite dst — that would be a different def
            }
            // Also rewrite terminator arg
            if (blk->jmp.arg == guarded)
                blk->jmp.arg = new_ref;

            // Rewrite uses in dominated blocks
            if (dt) {
                for (uint32_t dbi = 0; dbi < func->nblk; dbi++) {
                    if (dbi == bi) continue;
                    if (!xir_dom_covers(dt, bi, dbi)) continue;
                    XirBlock *dblk = func->blocks[dbi];
                    if (!dblk) continue;
                    for (uint32_t j = 0; j < dblk->nins; j++) {
                        XirIns *use = &dblk->ins[j];
                        for (int a = 0; a < 2; a++) {
                            if (use->args[a] == guarded)
                                use->args[a] = new_ref;
                        }
                    }
                    if (dblk->jmp.arg == guarded)
                        dblk->jmp.arg = new_ref;
                    // Rewrite PHI args in dominated blocks
                    for (XirPhi *phi = dblk->phis; phi; phi = phi->next) {
                        for (uint16_t p = 0; p < phi->narg; p++) {
                            if (phi->args[p] == guarded)
                                phi->args[p] = new_ref;
                        }
                    }
                }
            }
        }
    }
}

