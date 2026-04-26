/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_pass.h - XIR optimization pass pipeline
 *
 * KEY CONCEPT:
 *   Tiered optimization pipeline for JIT/AOT compilation.
 *   Tier 1 (-O0): minimal passes for fast compile time.
 *   Tier 2 (-O2): full optimization pipeline.
 *
 * RELATED MODULES:
 *   - xir.h: XIR data structures
 *   - xir_codegen.h: code generation (called after pipeline)
 */

#ifndef XIR_PASS_H
#define XIR_PASS_H

#include "xir.h"
#include "../base/xdefs.h"

/* ========== Pass Change Tracker ==========
 *
 * Fine-grained "what did this pass actually do?" record returned by
 * every optimisation pass.  The fixedpoint driver uses this to:
 *   1. Detect convergence — stop when no pass reports any change.
 *   2. Skip rebuilding analyses the pass did not affect (e.g. skip
 *      domtree rebuild when only ins_changed but not cfg_changed).
 */
typedef struct XirPassChange {
    bool cfg_changed;        // block/edge/terminator topology was altered
    bool vreg_defs_changed;  // instructions were added, deleted, or moved
    bool ins_changed;        // instruction args / flags rewritten in place
    uint32_t n_ins_removed;  // fine-grained counters used by heuristics
    uint32_t n_ins_added;
    uint32_t n_vregs_dead;
} XirPassChange;

/* Sentinel: "nothing changed" — shorthand for passes that early-exit. */
static inline XirPassChange xir_pass_no_change(void) {
    return (XirPassChange){false, false, false, 0, 0, 0};
}

/* Sentinel: "everything changed" — matches the conservative reset the
 * pipeline performs for legacy void passes.  Convenient for new
 * passes that touch mutation but have not yet grown fine-grained
 * bookkeeping. */
static inline XirPassChange xir_pass_change_all(void) {
    return (XirPassChange){true, true, true, 0, 0, 0};
}

// Forward declaration
typedef struct XrProto XrProto;

/* ========== Optimization Levels ========== */

typedef enum {
    XIR_OPT_NONE = 0,   // -O0: DCE only (Tier 1 JIT — fast compile)
    XIR_OPT_BASIC = 1,  // -O1: DCE + ConstProp + ConstFold
    XIR_OPT_FULL = 2,   // -O2: full pipeline (Tier 2 JIT / AOT)
} XirOptLevel;

/* ========== Pipeline API ========== */

// Run the optimization pipeline on a function at the given level
XR_FUNC void xir_run_pipeline(XirFunc *func, XirOptLevel opt);

// Extended pipeline with caller proto (enables auto-inlining at -O2)
XR_FUNC void xir_run_pipeline_ex(XirFunc *func, XirOptLevel opt, XrProto *proto);

/* ========== Declarative Pipeline Driver ==========
 *
 * xir_run_fixedpoint runs a sequence of passes repeatedly until no
 * more IR changes are observed (each pass returns XirPassChange;
 * the driver stops when no pass reports any mutation) or the
 * caller-supplied round cap is hit.  Passes are described by
 * XirPassDesc records so callers do not assemble a hand-written
 * chain of XIR_RUN_PASS macros anymore.
 *
 * The driver collects XirPassChange from each pass and invokes
 * XIR_RESET_ANALYSIS when the pass touched the IR.
 * Pass names are purely for logging (jit->verbose).
 */

typedef XirPassChange (*XirPassFn)(XirFunc *func);
typedef XirPassChange (*XirPassFnProto)(XirFunc *func, XrProto *proto);

/* Flags describing per-pass properties.  Currently advisory only;
 * used to drive logging and to pick the right verify macro
 * equivalent.  Future: fine-grained cache invalidation. */
#define XIR_PASS_NONE 0u
#define XIR_PASS_VERIFY_SE (1u << 0)        // DCE/CSE/GVN/copy_prop: SE count must not drop
#define XIR_PASS_NEEDS_PROTO (1u << 1)      // pass takes (func, proto)
#define XIR_PASS_SKIP_CFG_VERIFY (1u << 2)  // builder may leave pred lists dirty pre-CFG
#define XIR_PASS_NO_RESET (1u << 3)         // pass does not touch IR (purely analysis)

typedef struct XirPassDesc {
    const char *name;
    union {
        XirPassFn v;       // used when !NEEDS_PROTO
        XirPassFnProto p;  // used when NEEDS_PROTO
    } fn;
    uint32_t flags;
} XirPassDesc;

typedef struct XirPipelineStats {
    uint32_t rounds_run;   // number of complete sweeps performed
    uint32_t invocations;  // sum of passes executed across all rounds
    uint32_t converged;    // non-zero if driver stopped on convergence
    uint32_t timed_out;    // non-zero if budget deadline was exceeded
} XirPipelineStats;

/* Compile-time budget: caps pipeline wall-clock time so bg workers
 * are not blocked by pathological functions.  Pass NULL for no limit. */
typedef struct XirCompileBudget {
    uint64_t start_ns;     // pipeline start timestamp
    uint64_t deadline_ns;  // start_ns + budget (default 50ms)
    bool timed_out;        // set by driver when deadline exceeded
} XirCompileBudget;

/*
 * Run |passes| to a fixed-point on |func|, at most |max_rounds|
 * sweeps.  Returns per-run statistics so callers / tests can reason
 * about convergence behaviour.  Proto is optional; passes with
 * XIR_PASS_NEEDS_PROTO receive it, others ignore it.
 *
 * If |budget| is non-NULL the driver checks the deadline at the start
 * of every round and bails out early if exceeded.  The partially
 * optimised IR is still valid — only remaining passes are skipped.
 */
XR_FUNC XirPipelineStats xir_run_fixedpoint(XirFunc *func, XrProto *proto,
                                            const XirPassDesc *passes, uint32_t npass,
                                            uint32_t max_rounds, XirCompileBudget *budget);

/* ========== Individual Pass API ========== */

// Dead Code Elimination: remove instructions whose results are unused
XR_FUNC XirPassChange xir_pass_dce(XirFunc *func);

// Common Subexpression Elimination: replace duplicate pure computations (local, per-block)
XR_FUNC XirPassChange xir_pass_cse(XirFunc *func);

// Global Value Numbering: dominator-based cross-block CSE (replaces local CSE at -O2)
XR_FUNC XirPassChange xir_pass_gvn(XirFunc *func);

// Loop Invariant Code Motion: hoist invariant computations out of loops
XR_FUNC XirPassChange xir_pass_licm(XirFunc *func);

// Copy Propagation: replace uses of MOV dst with the original src,
// collapsing copy chains. Run before DCE to expose dead MOVs.
XR_FUNC XirPassChange xir_pass_copy_prop(XirFunc *func);

// Phi Simplification: replace phi nodes where all args are the same value
// (or all non-self args are the same) with a MOV from that value.
XR_FUNC XirPassChange xir_pass_phi_simp(XirFunc *func);

// Allocation Sinking: move XIR_ALLOC from dominating blocks into the
// specific branch where it's used, avoiding allocation on unused paths.
XR_FUNC XirPassChange xir_pass_alloc_sink(XirFunc *func);

// Global Code Motion (GCM): Click's algorithm to move pure instructions
// to optimal positions — as early as dependencies allow, but placed in the
// block with the lowest loop depth between early and late bounds.
XR_FUNC XirPassChange xir_pass_gcm(XirFunc *func);

// CFG Rebuild: reconstruct all predecessor lists from s1/s2 edges and
// remap phi args to match the new pred ordering. Call after any group of
// passes that modify CFG structure (sccp, merge_blocks, etc.).
XR_FUNC void xir_rebuild_preds(XirFunc *func);

// CFG Verification (debug only): assert pred/succ/phi consistency.
// Checks: every pred has blk as successor, every successor has blk as pred,
// phi->narg == npred, block terminator vs s1/s2, vreg/const range.
// No-op in release builds.
XR_FUNC void xir_verify_cfg(XirFunc *func);

// Type Verification (debug only): assert instruction type consistency.
// Checks: dst vreg type matches ins type, integer/float op arg types,
// BOX/UNBOX constraints, type conversion, constant types, ALLOC dst.
// No-op in release builds.
XR_FUNC void xir_verify_types(XirFunc *func);

// Critical Edge Splitting: insert empty blocks on edges from multi-successor
// blocks (BR) to multi-predecessor blocks. Enables clean phi resolution.
XR_FUNC XirPassChange xir_pass_split_critical_edges(XirFunc *func);

// Block Reordering: reorder func->blocks[] to minimize taken branches.
// Greedy fall-through chaining: BR false-branch as fall-through, loop bodies compact.
XR_FUNC XirPassChange xir_pass_reorder_blocks(XirFunc *func);

// Block Merging: merge a block with its sole successor when the successor
// has exactly one predecessor. Reduces jumps and expands optimization scope.
XR_FUNC XirPassChange xir_pass_merge_blocks(XirFunc *func);

// Store-to-Load Forwarding: when a LOAD_FIELD reads from the same object+offset
// that a preceding STORE_FIELD just wrote, replace the load with a MOV from
// the stored value. Also eliminates redundant loads (load-load forwarding).
XR_FUNC XirPassChange xir_pass_store_to_load(XirFunc *func);

// Redundant Guard Elimination: eliminate duplicate GUARD_TAG/GUARD_SHAPE/
// GUARD_NONNULL/GUARD_CLASS within each block. Useful after guard hoisting.
XR_FUNC XirPassChange xir_pass_elim_guards(XirFunc *func);

// Branch Value Propagation: when BR(cond) branches, propagate known values
// into single-predecessor successors. E.g., if cond = EQ(a, K) and the true
// successor has only one predecessor, replace uses of a with K in that block.
// Also propagates cond == 0 into the false successor.
XR_FUNC XirPassChange xir_pass_propjnz(XirFunc *func);

// If-Conversion: convert simple diamond CFG patterns into conditional select
// instructions (XIR_SELECT_COND + XIR_SELECT), eliminating branches.
// Pattern: BR(cond) → then/else → merge with PHI → SELECT + JMP merge
XR_FUNC XirPassChange xir_pass_ifconvert(XirFunc *func);

// TypePropagation: propagate precise XrType* through the IR.
// 1. GUARD_TAG narrows guarded vreg type in dominated blocks
// 2. Arithmetic result type inferred from operand types
// 3. Known CALL_KNOWN return types propagated to dst vreg
// Should run before select_rep for maximum type precision.
XR_FUNC XirPassChange xir_pass_type_prop(XirFunc *func);

// Type-driven specialization: lower polymorphic RT_* instructions to
// monomorphic native ops when type_prop has proven operand types.
// RT_LT/LE/EQ(I64,I64) → LT/LE/EQ, RT_LT(F64,F64) → FLT, etc.
// RT_ADD/SUB/MUL/DIV(F64,F64) → FADD/FSUB/FMUL/FDIV.
// Should run immediately after type_prop for maximum benefit.
XR_FUNC XirPassChange xir_pass_specialize(XirFunc *func);

// SelectRepresentations: eliminate redundant BOX/UNBOX pairs
XR_FUNC XirPassChange xir_pass_select_rep(XirFunc *func);

// InsertWriteBarriers: insert XIR_BARRIER_FWD after XIR_STORE_FIELD
XR_FUNC XirPassChange xir_insert_write_barriers(XirFunc *func);

// EliminateWriteBarriers: remove redundant BARRIER_FWD instructions.
// Rule 1: parent freshly allocated in same block (young → no barrier needed)
// Rule 2: duplicate barrier on same parent (already in remembered set)
// Both rules invalidated by GC-triggering instructions.
XR_FUNC XirPassChange xir_pass_elim_write_barriers(XirFunc *func);

// InsertArcReleases: AOT-only pass — insert XIR_RELEASE before each RET for
// locally-allocated PTR vregs that are not returned.
// Only used in AOT transpile mode (not JIT).
XR_FUNC XirPassChange xir_insert_arc_releases(XirFunc *func);

// Escape Analysis + Scalar Replacement: replace non-escaping XIR_ALLOC
// with virtual registers for each field (eliminates heap allocation).
// Per-block analysis: only optimizes allocations that don't escape the block.
XR_FUNC XirPassChange xir_pass_escape_analysis(XirFunc *func);

// Automatic Function Inlining: inline small XIR_CALL_KNOWN callees.
// caller_proto needed for recursion detection and callee XIR build.
XR_FUNC XirPassChange xir_pass_auto_inline(XirFunc *func, XrProto *caller_proto);

// Canonicalize: normalize IR for better optimization opportunities.
// 1. Commutative ops: ensure constant on right side (enables more CSE matches)
// 2. Identity elimination: x+0→x, x*1→x, x&-1→x, x|0→x, x^0→x
// 3. Absorbing elimination: x*0→0, x&0→0
// 4. Double negation: NEG(NEG(x))→x, NOT(NOT(x))→x
// Run after SCCP and before CSE for maximum effect.
XR_FUNC XirPassChange xir_pass_canonicalize(XirFunc *func);

// Dead Store Elimination: remove stores that are overwritten before being read.
// Per-block analysis: tracks (obj, offset) pairs; a store is dead if a later
// store to the same (obj, offset) exists with no intervening load or GC point.
XR_FUNC XirPassChange xir_pass_dse(XirFunc *func);

// REDEFINE Insertion: insert XIR_REDEFINE after GUARD instructions.
// Creates new SSA values with narrowed types for flow-sensitive type info.
// Rewrites dominated uses of the guarded vreg to the REDEFINE result.
XR_FUNC XirPassChange xir_pass_insert_redefines(XirFunc *func);

// Range Analysis: infer integer value ranges and eliminate redundant bounds checks.
// 1. Track loop induction variables: i=0; i<N; i++ → range [0, N-1]
// 2. Eliminate GUARD_BOUNDS where index range is provably within [0, length)
// 3. Propagate range constraints from GUARD_BOUNDS to dominated uses
XR_FUNC XirPassChange xir_pass_range_analysis(XirFunc *func);

/* ========== Function Inlining ========== */

// Inline callee into caller at a specific call site.
// call_block: block containing the call
// call_ins_idx: instruction index of the call within call_block
// callee: function to inline (must not be the same as caller)
// call_args: array of XirRef for callee parameters
// nargs: number of arguments
// Returns the XirRef of the inlined return value (or XIR_NONE on failure).
XR_FUNC XirRef xir_inline_function(XirFunc *caller, XirBlock *call_block, uint32_t call_ins_idx,
                                   XirFunc *callee, XirRef *call_args, uint32_t nargs);

#endif  // XIR_PASS_H
