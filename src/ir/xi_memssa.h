/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_memssa.h - Memory SSA for Xi IR
 *
 * KEY CONCEPT:
 *   Builds a parallel SSA graph for memory state.  Every store defines
 *   a new memory version; every load consumes the current version.
 *   Memory phis merge versions at CFG join points.
 *
 *   Combined with TBAA (xi_tbaa.h), this enables:
 *     - GVN-PRE to eliminate redundant loads across basic blocks
 *     - LICM to hoist/sink loads that are loop-invariant
 *     - Dead store elimination
 *
 * DESIGN:
 *   - Memory versions are dense uint32_t IDs within XiMemSSA
 *   - Each memory-accessing XiValue is linked to a XiMemAccess node
 *   - XiMemPhi nodes sit at CFG join points (parallel to Xi phis)
 *   - The memory graph is separate from the value graph — no XiValue
 *     is created for memory versions (keeps IR clean)
 *
 * INVARIANTS:
 *   - Requires XI_INV_TBAA_ANNOTATED (mem_group must be set)
 *   - After construction, XI_INV_MEM_SSA is set on the function
 *   - Memory SSA is invalidated when CFG or memory ops change
 *
 * LIFETIME:
 *   - XiMemSSA is allocated via xr_malloc and freed explicitly
 *   - It is NOT arena-allocated (outlives individual pass runs)
 */

#ifndef XI_MEMSSA_H
#define XI_MEMSSA_H

#include "xi.h"
#include "xi_tbaa.h"
#include "xi_pass.h"
#include "../base/xdefs.h"
#include <stdbool.h>

/* ========== Memory Version ========== */

/* A memory version represents a point in the memory state timeline.
 * Version 0 is the initial memory state at function entry. */
typedef uint32_t XiMemVer;

#define XI_MEMVER_ENTRY 0 /* initial memory state at function entry */
#define XI_MEMVER_INVALID UINT32_MAX

/* ========== Memory Access Node ==========
 *
 * Links a memory-accessing XiValue to its memory version.
 * - Loads consume a version (use_ver) but do not define one
 * - Stores both consume (use_ver) and define (def_ver) */
typedef struct XiMemAccess {
    XiValue *value;   /* the Xi value this access belongs to */
    XiMemVer use_ver; /* memory version consumed (input state) */
    XiMemVer def_ver; /* memory version defined (output state);
                       * XI_MEMVER_INVALID for loads */
} XiMemAccess;

/* ========== Memory Phi ==========
 *
 * Merges memory versions at CFG join points.
 * Parallel to XiPhi but operates on memory state. */
typedef struct XiMemPhi {
    XiBlock *block;        /* the block this phi belongs to */
    XiMemVer def_ver;      /* version defined by this phi */
    XiMemVer *pred_vers;   /* incoming versions (one per predecessor) */
    uint16_t npreds;       /* number of predecessors */
    struct XiMemPhi *next; /* linked list within XiMemSSA per block */
} XiMemPhi;

/* ========== Memory SSA Graph ========== */

typedef struct XiMemSSA {
    XiFunc *func; /* owning function */

    /* Per-value access nodes.  Indexed by XiValue.id.
     * NULL for non-memory values. */
    XiMemAccess **accesses;
    uint32_t naccesses; /* capacity = func->next_value_id */

    /* Per-block memory phis.  Indexed by XiBlock.id.
     * NULL for blocks with no memory phi. */
    XiMemPhi **block_phis;
    uint32_t nblock_phis; /* capacity = func->next_block_id */

    /* Version counter for allocation. */
    XiMemVer next_ver;
} XiMemSSA;

/* ========== Construction API ========== */

/* Build the Memory SSA graph for a function.
 * Requires XI_INV_TBAA_ANNOTATED to be set.
 * Returns NULL on allocation failure.
 * On success, sets XI_INV_MEM_SSA on the function. */
XR_FUNC XiMemSSA *xi_memssa_build(XiFunc *f);

/* Destroy a Memory SSA graph and free all resources. */
XR_FUNC void xi_memssa_destroy(XiMemSSA *mssa);

/* ========== Query API ========== */

/* Get the memory access node for a value (NULL if not a memory op). */
static inline XiMemAccess *xi_memssa_access(const XiMemSSA *mssa, const XiValue *v) {
    if (!mssa || !v || v->id >= mssa->naccesses)
        return NULL;
    return mssa->accesses[v->id];
}

/* Get the memory phi chain for a block (NULL if none). */
static inline XiMemPhi *xi_memssa_phis(const XiMemSSA *mssa, const XiBlock *blk) {
    if (!mssa || !blk || blk->id >= mssa->nblock_phis)
        return NULL;
    return mssa->block_phis[blk->id];
}

/* Check if two memory ops may alias through the memory SSA graph.
 * Combines TBAA group disjointness with version reachability. */
XR_FUNC bool xi_memssa_may_alias(const XiMemSSA *mssa, const XiValue *a, const XiValue *b);

/* Get the reaching definition (memory version) for a load.
 * Returns the XiValue that last wrote to the potentially-aliasing memory. */
XR_FUNC XiValue *xi_memssa_reaching_def(const XiMemSSA *mssa, const XiValue *load);

#endif  // XI_MEMSSA_H
