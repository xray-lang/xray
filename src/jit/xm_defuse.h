/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xm_defuse.h - Def-use chains for Xm virtual registers
 *
 * KEY CONCEPT:
 *   For each vreg, tracks:
 *     - def: the single defining instruction (already in XmVReg.def)
 *     - uses: all instruction/phi/terminator sites that reference this vreg
 *
 *   Uses are stored in a flat array with per-vreg offset/count indexing,
 *   enabling O(1) lookup of use count and O(n) iteration of uses.
 *
 * WHY THIS DESIGN:
 *   - Single allocation for the entire use array (cache-friendly)
 *   - Per-vreg offset/count enables direct access without linked lists
 *   - Immutable after construction (rebuild if IR changes)
 *
 * RELATED MODULES:
 *   - xm.h: Xm data structures (XmVReg.def for definitions)
 *   - xm_pass.h: optimization passes that consume def-use info
 */

#ifndef XM_DEFUSE_H
#define XM_DEFUSE_H

#include "xm.h"
#include "../base/xdefs.h"

// A single use site of a vreg
typedef struct {
    uint32_t blk;     // block index in func->blocks[]
    uint32_t ins;     // instruction index within block (UINT32_MAX for phi/terminator)
    uint8_t kind;     // XmUseKind: where in the instruction the vreg appears
    uint8_t arg_idx;  // argument index (0 or 1 for ins, phi arg index, 0 for jmp)
} XmUse;

typedef enum {
    XM_USE_INS_ARG = 0,  // instruction args[arg_idx]
    XM_USE_PHI_ARG = 1,  // phi node argument
    XM_USE_JMP_ARG = 2,  // terminator (BR cond / RET val)
} XmUseKind;

// Def-use chains for an entire function
typedef struct XmDefUse {
    XmUse *uses;         // flat array of all use records
    uint32_t *offset;     // offset[v] = start index in uses[] for vreg v
    uint32_t *count;      // count[v]  = number of uses for vreg v
    uint32_t nvreg;       // number of vregs
    uint32_t total_uses;  // total entries in uses[]
} XmDefUse;

/*
 * Build def-use chains for all vregs in func.
 * The result is a snapshot of current IR state; if IR is modified,
 * call xm_defuse_free() and rebuild.
 *
 * For most passes, prefer xm_func_get_defuse() below — it returns a
 * cached result that is reused across passes and invalidated
 * automatically by XM_RUN_PASS when IR is mutated.
 */
XR_FUNC void xm_defuse_build(XmDefUse *du, XmFunc *func);

// Release all memory held by a XmDefUse result
XR_FUNC void xm_defuse_free(XmDefUse *du);

/*
 * Lazy cache accessor: return the XmDefUse attached to |func|,
 * building it on first access.  The tree is owned by the function
 * and must not be freed by callers.  Returns NULL only when the
 * function has no vregs at all.
 */
XR_FUNC const XmDefUse *xm_func_get_defuse(XmFunc *func);

/*
 * Drop the cached def-use so the next xm_func_get_defuse() rebuilds
 * from scratch.  Safe to call when nothing is cached.  Called by the
 * XM_RUN_PASS harness after every pass so mutated IR is guaranteed
 * to produce a fresh chain on next access.
 */
XR_FUNC void xm_func_invalidate_defuse(XmFunc *func);

// Query: number of uses for vreg v
static inline uint32_t xm_defuse_nuses(const XmDefUse *du, uint32_t v) {
    if (v >= du->nvreg)
        return 0;
    return du->count[v];
}

// Query: get pointer to first use for vreg v (iterate count[v] entries)
static inline const XmUse *xm_defuse_uses(const XmDefUse *du, uint32_t v) {
    if (v >= du->nvreg || du->count[v] == 0)
        return NULL;
    return &du->uses[du->offset[v]];
}

// Query: is vreg v dead (zero uses)?
static inline bool xm_defuse_is_dead(const XmDefUse *du, uint32_t v) {
    return xm_defuse_nuses(du, v) == 0;
}

// Query: does vreg v have exactly one use?
static inline bool xm_defuse_single_use(const XmDefUse *du, uint32_t v) {
    return xm_defuse_nuses(du, v) == 1;
}

#endif  // XM_DEFUSE_H
