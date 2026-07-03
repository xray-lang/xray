/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_memssa.c - Memory SSA construction and query
 *
 * Builds a parallel SSA graph for memory state on top of the TBAA
 * annotations.  Construction uses a simplified renaming algorithm:
 *
 *   1. Walk blocks in RPO (reverse post-order).
 *   2. At each CFG join with >1 predecessor, insert a memory phi.
 *   3. For each instruction in the block:
 *      - If it is a memory store or call (defines new memory state),
 *        allocate a new version and record (use_ver, def_ver).
 *      - If it is a memory load, record (use_ver, INVALID).
 *   4. Propagate versions to successor blocks.
 *
 * The memory SSA is conservative: calls get XI_MEM_TOP and clobber
 * all memory groups, forcing a new version that subsumes all prior
 * versions at that point.
 */

#include "xi_memssa.h"
#include "../base/xmalloc.h"
#include "../base/xchecks.h"

#include <string.h>

/* ========== Internal Helpers ========== */

/* Allocate a fresh memory version. */
static XiMemVer alloc_ver(XiMemSSA *mssa) {
    XR_DCHECK(mssa != NULL, "alloc_ver: NULL mssa");
    XR_DCHECK(mssa->next_ver < XI_MEMVER_INVALID, "alloc_ver: version overflow");
    return mssa->next_ver++;
}

/* Create and register a memory access node for a value. */
static XiMemAccess *make_access(XiMemSSA *mssa, XiValue *v, XiMemVer use, XiMemVer def) {
    XR_DCHECK(mssa != NULL, "make_access: NULL mssa");
    XR_DCHECK(v != NULL, "make_access: NULL value");
    XR_DCHECK(v->id < mssa->naccesses, "make_access: value id out of range");

    XiMemAccess *acc = (XiMemAccess *) xr_malloc(sizeof(XiMemAccess));
    if (!acc)
        return NULL;

    acc->value = v;
    acc->use_ver = use;
    acc->def_ver = def;

    mssa->accesses[v->id] = acc;
    return acc;
}

/* Create a memory phi for a block. */
static XiMemPhi *make_mem_phi(XiMemSSA *mssa, XiBlock *blk, uint16_t npreds) {
    XR_DCHECK(mssa != NULL, "make_mem_phi: NULL mssa");
    XR_DCHECK(blk != NULL, "make_mem_phi: NULL block");
    XR_DCHECK(blk->id < mssa->nblock_phis, "make_mem_phi: block id out of range");

    XiMemPhi *phi = (XiMemPhi *) xr_malloc(sizeof(XiMemPhi));
    if (!phi)
        return NULL;

    phi->block = blk;
    phi->def_ver = alloc_ver(mssa);
    phi->npreds = npreds;
    phi->pred_vers = (XiMemVer *) xr_calloc(npreds, sizeof(XiMemVer));
    if (!phi->pred_vers) {
        xr_free(phi);
        return NULL;
    }

    /* Initialize pred versions to INVALID (filled during renaming). */
    for (uint16_t i = 0; i < npreds; i++)
        phi->pred_vers[i] = XI_MEMVER_INVALID;

    /* Prepend to block's memory phi chain. */
    phi->next = mssa->block_phis[blk->id];
    mssa->block_phis[blk->id] = phi;

    return phi;
}

/* Returns true if the op defines new memory state (store or call). */
static bool is_mem_def(uint16_t op) {
    if (xi_is_memory_store(op))
        return true;
    /* Calls may write arbitrary memory. */
    switch (op) {
        case XI_CALL:
        case XI_CALL_METHOD:
        case XI_CALL_METHOD_DIRECT:
        case XI_TAIL_CALL:
        case XI_CALL_BUILTIN:
        case XI_GO:
        case XI_THREAD_SPAWN:
        case XI_PRINT:
        case XI_THROW:
            return true;
        default:
            return false;
    }
}

/* Count predecessor blocks for a given block. */
static uint16_t count_preds(const XiBlock *blk) {
    XR_DCHECK(blk != NULL, "count_preds: NULL block");
    uint16_t n = 0;
    for (uint16_t i = 0; i < blk->npreds; i++) {
        if (blk->preds[i])
            n++;
    }
    return n;
}

/* ========== RPO Computation ========== */

/* Simple iterative RPO: walk blocks in order (XiFunc stores blocks
 * in approximate RPO already after lowering).  For correctness of
 * the renaming, we need a proper RPO.  Use a simple DFS. */

typedef struct {
    XiBlock **order;
    uint32_t count;
    bool *visited;
} RpoCtx;

static void rpo_dfs(RpoCtx *ctx, XiBlock *blk) {
    if (!blk || ctx->visited[blk->id])
        return;
    ctx->visited[blk->id] = true;

    /* Visit successors first (post-order). */
    if (blk->succs[0])
        rpo_dfs(ctx, blk->succs[0]);
    if (blk->succs[1])
        rpo_dfs(ctx, blk->succs[1]);

    /* Append in reverse (will be reversed later). */
    ctx->order[ctx->count++] = blk;
}

static XiBlock **compute_rpo(XiFunc *f, uint32_t *out_count) {
    XR_DCHECK(f != NULL, "compute_rpo: NULL func");

    uint32_t nblocks = f->next_block_id;
    XiBlock **order = (XiBlock **) xr_calloc(f->nblocks, sizeof(XiBlock *));
    bool *visited = (bool *) xr_calloc(nblocks, sizeof(bool));
    if (!order || !visited) {
        xr_free(order);
        xr_free(visited);
        *out_count = 0;
        return NULL;
    }

    RpoCtx ctx = {order, 0, visited};
    rpo_dfs(&ctx, f->entry);

    /* Reverse the post-order array to get RPO. */
    for (uint32_t i = 0; i < ctx.count / 2; i++) {
        XiBlock *tmp = order[i];
        order[i] = order[ctx.count - 1 - i];
        order[ctx.count - 1 - i] = tmp;
    }

    xr_free(visited);
    *out_count = ctx.count;
    return order;
}

/* ========== Construction ========== */

XR_FUNC XiMemSSA *xi_memssa_build(XiFunc *f) {
    XR_DCHECK(f != NULL, "xi_memssa_build: NULL func");
    XR_DCHECK(f->invariant_mask & XI_INV_TBAA_ANNOTATED,
              "xi_memssa_build: requires XI_INV_TBAA_ANNOTATED");

    XiMemSSA *mssa = (XiMemSSA *) xr_calloc(1, sizeof(XiMemSSA));
    if (!mssa)
        return NULL;

    mssa->func = f;
    mssa->naccesses = f->next_value_id;
    mssa->nblock_phis = f->next_block_id;
    mssa->next_ver = 1; /* version 0 = entry state */

    mssa->accesses = (XiMemAccess **) xr_calloc(mssa->naccesses, sizeof(XiMemAccess *));
    mssa->block_phis = (XiMemPhi **) xr_calloc(mssa->nblock_phis, sizeof(XiMemPhi *));
    if (!mssa->accesses || !mssa->block_phis) {
        xi_memssa_destroy(mssa);
        return NULL;
    }

    /* Per-block current memory version (for renaming). */
    XiMemVer *cur_ver = (XiMemVer *) xr_calloc(mssa->nblock_phis, sizeof(XiMemVer));
    if (!cur_ver) {
        xi_memssa_destroy(mssa);
        return NULL;
    }

    /* Compute RPO. */
    uint32_t rpo_count = 0;
    XiBlock **rpo = compute_rpo(f, &rpo_count);
    if (!rpo) {
        xr_free(cur_ver);
        xi_memssa_destroy(mssa);
        return NULL;
    }

    /* Initialize entry block version. */
    if (f->entry)
        cur_ver[f->entry->id] = XI_MEMVER_ENTRY;

    /* Walk blocks in RPO and rename memory versions. */
    for (uint32_t ri = 0; ri < rpo_count; ri++) {
        XiBlock *blk = rpo[ri];
        XR_DCHECK(blk != NULL, "xi_memssa_build: NULL block in RPO");

        uint16_t npreds = count_preds(blk);

        /* Insert memory phi at join points. */
        XiMemVer live_ver;
        if (npreds > 1) {
            XiMemPhi *mphi = make_mem_phi(mssa, blk, npreds);
            if (!mphi) {
                xr_free(cur_ver);
                xr_free(rpo);
                xi_memssa_destroy(mssa);
                return NULL;
            }
            /* Fill predecessor versions from cur_ver. */
            for (uint16_t pi = 0; pi < blk->npreds && pi < npreds; pi++) {
                XiBlock *pred = blk->preds[pi];
                if (pred && pred->id < mssa->nblock_phis)
                    mphi->pred_vers[pi] = cur_ver[pred->id];
            }
            live_ver = mphi->def_ver;
        } else if (npreds == 1) {
            /* Single predecessor: inherit its version. */
            XiBlock *pred = blk->preds[0];
            live_ver = (pred && pred->id < mssa->nblock_phis) ? cur_ver[pred->id] : XI_MEMVER_ENTRY;
        } else {
            /* Entry block or unreachable. */
            live_ver = (blk == f->entry) ? XI_MEMVER_ENTRY : XI_MEMVER_INVALID;
        }

        /* Rename instructions in block order. */
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            XiValue *v = blk->values[vi];
            if (!v)
                continue;

            /* Skip non-memory ops. */
            if (v->mem_group == XI_MEM_NONE && !is_mem_def(v->op))
                continue;

            if (is_mem_def(v->op)) {
                /* Store/call: consumes current version, defines new one. */
                XiMemVer new_ver = alloc_ver(mssa);
                make_access(mssa, v, live_ver, new_ver);
                live_ver = new_ver;
            } else if (xi_is_memory_load(v->op)) {
                /* Load: consumes current version, does not define. */
                make_access(mssa, v, live_ver, XI_MEMVER_INVALID);
            }
        }

        /* Record outgoing version for this block. */
        cur_ver[blk->id] = live_ver;
    }

    xr_free(cur_ver);
    xr_free(rpo);

    f->invariant_mask |= XI_INV_MEM_SSA;
    return mssa;
}

/* ========== Destruction ========== */

XR_FUNC void xi_memssa_destroy(XiMemSSA *mssa) {
    if (!mssa)
        return;

    /* Free access nodes. */
    if (mssa->accesses) {
        for (uint32_t i = 0; i < mssa->naccesses; i++)
            xr_free(mssa->accesses[i]);
        xr_free(mssa->accesses);
    }

    /* Free memory phis. */
    if (mssa->block_phis) {
        for (uint32_t i = 0; i < mssa->nblock_phis; i++) {
            XiMemPhi *phi = mssa->block_phis[i];
            while (phi) {
                XiMemPhi *next = phi->next;
                xr_free(phi->pred_vers);
                xr_free(phi);
                phi = next;
            }
        }
        xr_free(mssa->block_phis);
    }

    xr_free(mssa);
}

/* ========== Query Implementation ========== */

XR_FUNC bool xi_memssa_may_alias(const XiMemSSA *mssa, const XiValue *a, const XiValue *b) {
    XR_DCHECK(mssa != NULL, "xi_memssa_may_alias: NULL mssa");
    XR_DCHECK(a != NULL, "xi_memssa_may_alias: NULL a");
    XR_DCHECK(b != NULL, "xi_memssa_may_alias: NULL b");

    /* First check TBAA: if groups are provably disjoint, no alias. */
    if (!xi_tbaa_may_alias(a, b))
        return false;

    /* TBAA says "may alias" — check memory versions.
     * If both are loads consuming the same version with no intervening
     * store, they read the same value (redundant load). */
    XiMemAccess *aa = xi_memssa_access(mssa, a);
    XiMemAccess *ab = xi_memssa_access(mssa, b);
    if (!aa || !ab)
        return false; /* non-memory — no alias */

    /* Conservative: if TBAA says may-alias, trust it.
     * Version-based refinement can be added later for
     * must-alias (redundant load elimination). */
    return true;
}

XR_FUNC XiValue *xi_memssa_reaching_def(const XiMemSSA *mssa, const XiValue *load) {
    XR_DCHECK(mssa != NULL, "xi_memssa_reaching_def: NULL mssa");
    XR_DCHECK(load != NULL, "xi_memssa_reaching_def: NULL load");

    XiMemAccess *acc = xi_memssa_access(mssa, load);
    if (!acc || acc->use_ver == XI_MEMVER_ENTRY || acc->use_ver == XI_MEMVER_INVALID)
        return NULL;

    /* Walk backward through accesses to find the store that defined use_ver.
     * This is O(n) in the worst case; a version→value index can be added
     * if profiling shows this is a bottleneck. */
    XiMemVer target = acc->use_ver;
    for (uint32_t i = 0; i < mssa->naccesses; i++) {
        XiMemAccess *other = mssa->accesses[i];
        if (other && other->def_ver == target)
            return other->value;
    }

    return NULL;
}
