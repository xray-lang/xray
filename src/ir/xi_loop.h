/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_loop.h - Natural-loop forest for Xi IR
 *
 * KEY CONCEPT:
 *   Identifies all natural loops in a function's CFG using the dominator
 *   tree.  A back-edge (src -> hdr) exists when hdr dominates src.
 *   The natural-loop body is all blocks reachable backward from the latch
 *   without crossing the header.
 *
 * GUARANTEES:
 *   1. Every XiLoop has a non-null header and at least one latch.
 *   2. all_loops[] is sorted innermost-first (children before parents)
 *      so loop-local transforms process inner loops before outer ones.
 *   3. block_to_loop[bid] is the innermost enclosing loop for block bid.
 *   4. preheader is set only when the header has exactly one out-of-loop
 *      predecessor; NULL otherwise.
 *
 * INVARIANTS:
 *   Requires RPO + dominator tree computed via xi_analysis.h.
 *
 *   Two ownership models coexist:
 *
 *   - xi_compute_loops() returns a heap-allocated XiLoopInfo that
 *     the caller must release with xi_loopinfo_free().  This entry
 *     point is unconditional — every call re-derives the forest.
 *
 *   - xi_ensure_loops() returns a borrowed pointer owned by the
 *     XiFunc.  It lazily reuses the cached forest when cfg_version
 *     has not advanced since the last computation.  The cache is
 *     freed by xi_func_free or transparently rebuilt when a CFG
 *     change invalidates it.
 */

#ifndef XI_LOOP_H
#define XI_LOOP_H

#include "xi.h"
#include <stdint.h>
#include <stdbool.h>

/* ========== Loop Structures ========== */

typedef enum XiIvKind {
    XI_IV_BASIC = 1,
    XI_IV_DERIVED = 2,
    XI_IV_POLYNOMIAL = 3,
} XiIvKind;

typedef struct XiBasicIV {
    XiValue *phi;       /* loop-carried phi value */
    XiValue *start;     /* incoming value from preheader */
    XiValue *next;      /* incoming value from latch */
    XiValue *step;      /* loop-invariant step operand */
    XiOp step_op;       /* XI_ADD or XI_SUB */
    int64_t step_const; /* canonical signed step when step is const */
    bool has_const_step;
} XiBasicIV;

typedef struct XiDerivedIV {
    XiValue *value;      /* derived expression value */
    XiValue *base;       /* basic IV phi value */
    XiValue *scale;      /* loop-invariant scale, NULL means 1 */
    XiValue *offset;     /* loop-invariant offset, NULL means 0 */
    int64_t scale_const; /* known scale when constant */
    int64_t offset_const;
    bool has_const_scale;
    bool has_const_offset;
} XiDerivedIV;

typedef struct XiPolynomialIV {
    XiValue *value; /* polynomial expression value */
    XiValue *base;  /* basic IV phi value */
} XiPolynomialIV;

typedef struct XiLoop {
    struct XiLoop *parent;  /* outer loop (NULL at forest root) */
    struct XiLoop *child;   /* first nested loop */
    struct XiLoop *sibling; /* next loop sharing the same parent */

    XiBlock *header;    /* loop header block */
    XiBlock *preheader; /* unique out-of-loop predecessor, or NULL */
    XiBlock *latch;     /* back-edge source (first if multiple) */

    XiBlock **body; /* heap-allocated body block array */
    uint32_t nbody; /* number of blocks in body */

    uint32_t depth; /* 1-based nesting depth */
    uint32_t id;    /* index in XiLoopInfo::all_loops[] */

    XiBasicIV *basic_ivs;
    uint32_t nbasic_ivs;
    XiDerivedIV *derived_ivs;
    uint32_t nderived_ivs;
    XiPolynomialIV *polynomial_ivs;
    uint32_t npolynomial_ivs;
} XiLoop;

typedef struct XiLoopInfo {
    XiLoop *root_list;      /* top-level loops linked via sibling */
    XiLoop **all_loops;     /* heap array, innermost-first */
    XiLoop **block_to_loop; /* [nblocks] innermost loop per block */
    uint32_t nloop;         /* total number of loops */
    uint32_t nblocks;       /* size of block_to_loop[] */
} XiLoopInfo;

/* ========== API ========== */

/* Compute the natural-loop forest for the function.
 * Requires RPO + dominators already computed — call xi_ensure_dominators(f)
 * first (or the unconditional xi_compute_rpo / xi_compute_dominators pair).
 * Returns heap-allocated result, or NULL when there are no loops.
 * Caller owns the result; release with xi_loopinfo_free. */
XR_FUNC XiLoopInfo *xi_compute_loops(XiFunc *f);

/* Cached form: returns the borrowed XiLoopInfo* owned by f.
 * Recomputes and replaces the cache only when f->loop_version lags
 * f->cfg_version.  Returns NULL when the function has no loops
 * (cached as a sentinel — subsequent calls remain hits).
 * Callers MUST NOT free the result. */
XR_FUNC XiLoopInfo *xi_ensure_loops(XiFunc *f);

/* Free loop info and all associated memory.
 * Only call this on the result of xi_compute_loops; never on the
 * pointer returned by xi_ensure_loops. */
XR_FUNC void xi_loopinfo_free(XiLoopInfo *info);

/* Query: innermost loop nesting depth for block.
 * Returns 0 for blocks outside any loop. */
XR_FUNC uint32_t xi_block_loop_depth(const XiLoopInfo *info, uint32_t blk_id);

/* Query: does the given block belong to the given loop? */
XR_FUNC bool xi_loop_contains_block(const XiLoop *loop, const XiBlock *blk);

#endif  // XI_LOOP_H
