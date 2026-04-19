/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_defuse.h - Def-use chains for XIR virtual registers
 *
 * KEY CONCEPT:
 *   For each vreg, tracks:
 *     - def: the single defining instruction (already in XirVReg.def)
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
 *   - xir.h: XIR data structures (XirVReg.def for definitions)
 *   - xir_pass.h: optimization passes that consume def-use info
 */

#ifndef XIR_DEFUSE_H
#define XIR_DEFUSE_H

#include "xir.h"
#include "../base/xdefs.h"

// A single use site of a vreg
typedef struct {
    uint32_t blk; // block index in func->blocks[]
    uint32_t ins; // instruction index within block (UINT32_MAX for phi/terminator)
    uint8_t  kind; // XirUseKind: where in the instruction the vreg appears
    uint8_t  arg_idx; // argument index (0 or 1 for ins, phi arg index, 0 for jmp)
} XirUse;

typedef enum {
    XIR_USE_INS_ARG  = 0, // instruction args[arg_idx]
    XIR_USE_PHI_ARG  = 1, // phi node argument
    XIR_USE_JMP_ARG  = 2, // terminator (BR cond / RET val)
} XirUseKind;

// Def-use chains for an entire function
typedef struct {
    XirUse   *uses; // flat array of all use records
    uint32_t *offset; // offset[v] = start index in uses[] for vreg v
    uint32_t *count; // count[v]  = number of uses for vreg v
    uint32_t  nvreg; // number of vregs
    uint32_t  total_uses; // total entries in uses[]
} XirDefUse;

/*
 * Build def-use chains for all vregs in func.
 * The result is a snapshot of current IR state; if IR is modified,
 * call xir_defuse_free() and rebuild.
 *
 * For most passes, prefer xir_func_get_defuse() below — it returns a
 * cached result that is reused across passes and invalidated
 * automatically by XIR_RUN_PASS when IR is mutated.
 */
XR_FUNC void xir_defuse_build(XirDefUse *du, XirFunc *func);

// Release all memory held by a XirDefUse result
XR_FUNC void xir_defuse_free(XirDefUse *du);

/*
 * Lazy cache accessor: return the XirDefUse attached to |func|,
 * building it on first access.  The tree is owned by the function
 * and must not be freed by callers.  Returns NULL only when the
 * function has no vregs at all.
 */
XR_FUNC const XirDefUse *xir_func_get_defuse(XirFunc *func);

/*
 * Drop the cached def-use so the next xir_func_get_defuse() rebuilds
 * from scratch.  Safe to call when nothing is cached.  Called by the
 * XIR_RUN_PASS harness after every pass so mutated IR is guaranteed
 * to produce a fresh chain on next access.
 */
XR_FUNC void xir_func_invalidate_defuse(XirFunc *func);

// Query: number of uses for vreg v
static inline uint32_t xir_defuse_nuses(const XirDefUse *du, uint32_t v) {
    if (v >= du->nvreg) return 0;
    return du->count[v];
}

// Query: get pointer to first use for vreg v (iterate count[v] entries)
static inline const XirUse *xir_defuse_uses(const XirDefUse *du, uint32_t v) {
    if (v >= du->nvreg || du->count[v] == 0) return NULL;
    return &du->uses[du->offset[v]];
}

// Query: is vreg v dead (zero uses)?
static inline bool xir_defuse_is_dead(const XirDefUse *du, uint32_t v) {
    return xir_defuse_nuses(du, v) == 0;
}

// Query: does vreg v have exactly one use?
static inline bool xir_defuse_single_use(const XirDefUse *du, uint32_t v) {
    return xir_defuse_nuses(du, v) == 1;
}

#endif // XIR_DEFUSE_H
