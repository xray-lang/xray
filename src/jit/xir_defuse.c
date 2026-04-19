/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_defuse.c - Def-use chain construction for XIR
 *
 * KEY CONCEPT:
 *   Two-pass algorithm:
 *     Pass 1: count uses per vreg
 *     Pass 2: fill use records into pre-allocated flat array
 *   This avoids linked lists and gives cache-friendly iteration.
 */

#include "xir_defuse.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include <string.h>

// Pass 1 helper: increment use count for a vreg ref
static inline void count_ref(uint32_t *cnt, uint32_t nv, XirRef ref) {
    if (xir_ref_is_vreg(ref)) {
        uint32_t idx = XIR_REF_INDEX(ref);
        if (idx < nv) cnt[idx]++;
    }
}

// Pass 2 helper: record a use and advance the write cursor
static inline void record_use(XirDefUse *du, uint32_t vreg,
                               uint32_t blk, uint32_t ins,
                               uint8_t kind, uint8_t arg_idx) {
    uint32_t pos = du->offset[vreg] + du->count[vreg];
    du->uses[pos].blk = blk;
    du->uses[pos].ins = ins;
    du->uses[pos].kind = kind;
    du->uses[pos].arg_idx = arg_idx;
    du->count[vreg]++;
}

void xir_defuse_build(XirDefUse *du, XirFunc *func) {
    if (!du || !func) return;
    memset(du, 0, sizeof(*du));

    uint32_t nv = func->nvreg;
    if (nv == 0) return;

    du->nvreg = nv;
    du->count = xr_calloc(nv, sizeof(uint32_t));
    du->offset = xr_calloc(nv, sizeof(uint32_t));

    // ---- Pass 1: count uses per vreg ----
    uint32_t total = 0;
    for (uint32_t bi = 0; bi < func->nblk; bi++) {
        XirBlock *blk = func->blocks[bi];

        // Instruction args
        for (uint32_t i = 0; i < blk->nins; i++) {
            XirIns *ins = &blk->ins[i];
            for (int a = 0; a < 2; a++)
                count_ref(du->count, nv, ins->args[a]);
        }

        // Phi args
        for (XirPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint32_t p = 0; p < phi->narg; p++)
                count_ref(du->count, nv, phi->args[p]);
        }

        // Terminator arg
        count_ref(du->count, nv, blk->jmp.arg);
    }

    // Deopt info slot refs: keep deopt-referenced vregs alive
    for (uint32_t d = 0; d < func->ndeopt; d++) {
        XirDeoptInfo *info = &func->deopt_infos[d];
        for (uint16_t s = 0; s < info->nslots; s++)
            count_ref(du->count, nv, info->slots[s].value);
    }

    // Call arg pool refs: vregs used as CALL_C/CALL_KNOWN arguments
    if (func->call_arg_pool) {
        for (uint32_t v = 0; v < nv; v++) {
            if (func->vregs[v].call_nargs == 0) continue;
            uint32_t start = func->vregs[v].call_arg_start;
            for (uint16_t a = 0; a < func->vregs[v].call_nargs; a++)
                count_ref(du->count, nv, func->call_arg_pool[start + a]);
        }
    }

    // Compute offsets (prefix sum)
    for (uint32_t v = 0; v < nv; v++) {
        du->offset[v] = total;
        total += du->count[v];
    }
    du->total_uses = total;

    // Allocate flat use array
    if (total == 0) {
        du->uses = NULL;
        // Reset counts to zero for Pass 2 (already zero)
        return;
    }

    du->uses = xr_calloc(total, sizeof(XirUse));

    // Reset counts to zero — Pass 2 will re-count as write cursors
    memset(du->count, 0, nv * sizeof(uint32_t));

    // ---- Pass 2: fill use records ----
    for (uint32_t bi = 0; bi < func->nblk; bi++) {
        XirBlock *blk = func->blocks[bi];

        // Instruction args
        for (uint32_t i = 0; i < blk->nins; i++) {
            XirIns *ins = &blk->ins[i];
            for (int a = 0; a < 2; a++) {
                if (xir_ref_is_vreg(ins->args[a])) {
                    uint32_t idx = XIR_REF_INDEX(ins->args[a]);
                    if (idx < nv)
                        record_use(du, idx, bi, i, XIR_USE_INS_ARG, (uint8_t)a);
                }
            }
        }

        // Phi args
        for (XirPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint32_t p = 0; p < phi->narg; p++) {
                if (xir_ref_is_vreg(phi->args[p])) {
                    uint32_t idx = XIR_REF_INDEX(phi->args[p]);
                    if (idx < nv)
                        record_use(du, idx, bi, UINT32_MAX, XIR_USE_PHI_ARG, (uint8_t)p);
                }
            }
        }

        // Terminator arg
        if (xir_ref_is_vreg(blk->jmp.arg)) {
            uint32_t idx = XIR_REF_INDEX(blk->jmp.arg);
            if (idx < nv)
                record_use(du, idx, bi, UINT32_MAX, XIR_USE_JMP_ARG, 0);
        }
    }

    // Deopt info slot refs
    for (uint32_t d = 0; d < func->ndeopt; d++) {
        XirDeoptInfo *info = &func->deopt_infos[d];
        for (uint16_t s = 0; s < info->nslots; s++) {
            if (xir_ref_is_vreg(info->slots[s].value)) {
                uint32_t idx = XIR_REF_INDEX(info->slots[s].value);
                if (idx < nv)
                    record_use(du, idx, UINT32_MAX, UINT32_MAX, XIR_USE_JMP_ARG, 0);
            }
        }
    }

    // Call arg pool refs
    if (func->call_arg_pool) {
        for (uint32_t v = 0; v < nv; v++) {
            if (func->vregs[v].call_nargs == 0) continue;
            uint32_t start = func->vregs[v].call_arg_start;
            for (uint16_t a = 0; a < func->vregs[v].call_nargs; a++) {
                XirRef ref = func->call_arg_pool[start + a];
                if (xir_ref_is_vreg(ref)) {
                    uint32_t idx = XIR_REF_INDEX(ref);
                    if (idx < nv)
                        record_use(du, idx, UINT32_MAX, UINT32_MAX, XIR_USE_INS_ARG, 0);
                }
            }
        }
    }
}

void xir_defuse_free(XirDefUse *du) {
    if (!du) return;
    xr_free(du->uses);
    xr_free(du->offset);
    xr_free(du->count);
    memset(du, 0, sizeof(*du));
}

const XirDefUse *xir_func_get_defuse(XirFunc *func) {
    if (!func || func->nvreg == 0) return NULL;
    if (func->defuse) return func->defuse;

    XirDefUse *du = (XirDefUse *)xr_calloc(1, sizeof(XirDefUse));
    if (!du) return NULL;
    xir_defuse_build(du, func);
    func->defuse = du;
    return du;
}

void xir_func_invalidate_defuse(XirFunc *func) {
    if (!func || !func->defuse) return;
    xir_defuse_free(func->defuse);
    xr_free(func->defuse);
    func->defuse = NULL;
}
