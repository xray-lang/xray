/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xm_pass.h - Xm optimization pass pipeline
 *
 * KEY CONCEPT:
 *   Tiered optimization pipeline for JIT/AOT compilation.
 *   Tier 1 (-O0): minimal passes for fast compile time.
 *   Tier 2 (-O2): full optimization pipeline.
 *
 * RELATED MODULES:
 *   - xm.h: Xm data structures
 *   - xm_codegen.h: code generation (called after pipeline)
 */

#ifndef XM_PASS_H
#define XM_PASS_H

#include "xm.h"
#include "../base/xdefs.h"

/* ========== Pass Change Tracker ==========
 *
 * Fine-grained "what did this pass actually do?" record returned by
 * every optimisation pass.  The fixedpoint driver uses this to:
 *   1. Detect convergence — stop when no pass reports any change.
 *   2. Skip rebuilding analyses the pass did not affect (e.g. skip
 *      domtree rebuild when only ins_changed but not cfg_changed).
 */
typedef struct XmPassChange {
    bool cfg_changed;        // block/edge/terminator topology was altered
    bool vreg_defs_changed;  // instructions were added, deleted, or moved
    bool ins_changed;        // instruction args / flags rewritten in place
    uint32_t n_ins_removed;  // fine-grained counters used by heuristics
    uint32_t n_ins_added;
    uint32_t n_vregs_dead;
} XmPassChange;

/* Sentinel: "nothing changed" — shorthand for passes that early-exit. */
static inline XmPassChange xm_pass_no_change(void) {
    return (XmPassChange) {false, false, false, 0, 0, 0};
}

/* Sentinel: "everything changed" — matches the conservative reset the
 * pipeline performs for legacy void passes.  Convenient for new
 * passes that touch mutation but have not yet grown fine-grained
 * bookkeeping. */
static inline XmPassChange xm_pass_change_all(void) {
    return (XmPassChange) {true, true, true, 0, 0, 0};
}

// Forward declaration
typedef struct XrProto XrProto;

/* ========== Optimization Levels ========== */

typedef enum {
    XM_OPT_NONE = 0,   // -O0: DCE only (Tier 1 JIT — fast compile)
    XM_OPT_BASIC = 1,  // -O1: DCE + ConstProp + ConstFold
    XM_OPT_FULL = 2,   // -O2: full pipeline (Tier 2 JIT / AOT)
} XmOptLevel;

/* ========== Pipeline API ========== */

// Run the optimization pipeline on a function at the given level
XR_FUNC void xm_run_pipeline(XmFunc *func, XmOptLevel opt);

// Extended pipeline with caller proto (enables auto-inlining at -O2).
// budget_ms: wall-clock cap in milliseconds (0 = unlimited).
// Returns true if the budget was exceeded (caller should bail out).
XR_FUNC bool xm_run_pipeline_ex(XmFunc *func, XmOptLevel opt, XrProto *proto, uint32_t budget_ms);

// Budget constants (milliseconds)
#define XM_BUDGET_SYNC_MS 10  // Tier 1 JIT on main thread
#define XM_BUDGET_BG_MS 50    // background worker compilation

/* ========== Declarative Pipeline Driver ==========
 *
 * xm_run_fixedpoint runs a sequence of passes repeatedly until no
 * more IR changes are observed (each pass returns XmPassChange;
 * the driver stops when no pass reports any mutation) or the
 * caller-supplied round cap is hit.  Passes are described by
 * XmPassDesc records so callers do not assemble a hand-written
 * chain of XM_RUN_PASS macros anymore.
 *
 * The driver collects XmPassChange from each pass and invokes
 * XM_RESET_ANALYSIS when the pass touched the IR.
 * Pass names are purely for logging (jit->verbose).
 */

typedef XmPassChange (*XmPassFn)(XmFunc *func);
typedef XmPassChange (*XmPassFnProto)(XmFunc *func, XrProto *proto);

/* Flags describing per-pass properties.  Currently advisory only;
 * used to drive logging and to pick the right verify macro
 * equivalent.  Future: fine-grained cache invalidation. */
#define XM_PASS_NONE 0u
#define XM_PASS_VERIFY_SE (1u << 0)        // DCE/CSE/GVN/copy_prop: SE count must not drop
#define XM_PASS_NEEDS_PROTO (1u << 1)      // pass takes (func, proto)
#define XM_PASS_SKIP_CFG_VERIFY (1u << 2)  // builder may leave pred lists dirty pre-CFG
#define XM_PASS_NO_RESET (1u << 3)         // pass does not touch IR (purely analysis)

typedef struct XmPassDesc {
    const char *name;
    union {
        XmPassFn v;       // used when !NEEDS_PROTO
        XmPassFnProto p;  // used when NEEDS_PROTO
    } fn;
    uint32_t flags;
} XmPassDesc;

typedef struct XmPipelineStats {
    uint32_t rounds_run;   // number of complete sweeps performed
    uint32_t invocations;  // sum of passes executed across all rounds
    uint32_t converged;    // non-zero if driver stopped on convergence
    uint32_t timed_out;    // non-zero if budget deadline was exceeded
} XmPipelineStats;

/* Compile-time budget: caps pipeline wall-clock time so bg workers
 * are not blocked by pathological functions.  Pass NULL for no limit. */
typedef struct XmCompileBudget {
    uint64_t start_ns;     // pipeline start timestamp
    uint64_t deadline_ns;  // start_ns + budget (default 50ms)
    bool timed_out;        // set by driver when deadline exceeded
} XmCompileBudget;

/*
 * Run |passes| to a fixed-point on |func|, at most |max_rounds|
 * sweeps.  Returns per-run statistics so callers / tests can reason
 * about convergence behaviour.  Proto is optional; passes with
 * XM_PASS_NEEDS_PROTO receive it, others ignore it.
 *
 * If |budget| is non-NULL the driver checks the deadline at the start
 * of every round and bails out early if exceeded.  The partially
 * optimised IR is still valid — only remaining passes are skipped.
 */
XR_FUNC XmPipelineStats xm_run_fixedpoint(XmFunc *func, XrProto *proto, const XmPassDesc *passes,
                                          uint32_t npass, uint32_t max_rounds,
                                          XmCompileBudget *budget);

/* ========== Individual Pass API ========== */

// Dead Code Elimination: remove instructions whose results are unused
XR_FUNC XmPassChange xm_pass_dce(XmFunc *func);

// Common Subexpression Elimination: replace duplicate pure computations (local, per-block)
XR_FUNC XmPassChange xm_pass_cse(XmFunc *func);

// Global Value Numbering: dominator-based cross-block CSE (replaces local CSE at -O2)
XR_FUNC XmPassChange xm_pass_gvn(XmFunc *func);

// Loop Invariant Code Motion: hoist invariant computations out of loops
XR_FUNC XmPassChange xm_pass_licm(XmFunc *func);

// Copy Propagation: replace uses of MOV dst with the original src,
// collapsing copy chains. Run before DCE to expose dead MOVs.
XR_FUNC XmPassChange xm_pass_copy_prop(XmFunc *func);

// Phi Simplification: replace phi nodes where all args are the same value
// (or all non-self args are the same) with a MOV from that value.
XR_FUNC XmPassChange xm_pass_phi_simp(XmFunc *func);

// Allocation Sinking: move XM_ALLOC from dominating blocks into the
// specific branch where it's used, avoiding allocation on unused paths.
XR_FUNC XmPassChange xm_pass_alloc_sink(XmFunc *func);

// Global Code Motion (GCM): Click's algorithm to move pure instructions
// to optimal positions — as early as dependencies allow, but placed in the
// block with the lowest loop depth between early and late bounds.
XR_FUNC XmPassChange xm_pass_gcm(XmFunc *func);

// CFG Rebuild: reconstruct all predecessor lists from s1/s2 edges and
// remap phi args to match the new pred ordering. Call after any group of
// passes that modify CFG structure (sccp, merge_blocks, etc.).
XR_FUNC void xm_rebuild_preds(XmFunc *func);

// CFG Verification (debug only): assert pred/succ/phi consistency.
// Checks: every pred has blk as successor, every successor has blk as pred,
// phi->narg == npred, block terminator vs s1/s2, vreg/const range.
// No-op in release builds.
XR_FUNC void xm_verify_cfg(XmFunc *func);

// Type Verification (debug only): assert instruction type consistency.
// Checks: dst vreg type matches ins type, integer/float op arg types,
// BOX/UNBOX constraints, type conversion, constant types, ALLOC dst.
// No-op in release builds.
XR_FUNC void xm_verify_types(XmFunc *func);

// Critical Edge Splitting: insert empty blocks on edges from multi-successor
// blocks (BR) to multi-predecessor blocks. Enables clean phi resolution.
XR_FUNC XmPassChange xm_pass_split_critical_edges(XmFunc *func);

// Block Reordering: reorder func->blocks[] to minimize taken branches.
// Greedy fall-through chaining: BR false-branch as fall-through, loop bodies compact.
XR_FUNC XmPassChange xm_pass_reorder_blocks(XmFunc *func);

// Block Merging: merge a block with its sole successor when the successor
// has exactly one predecessor. Reduces jumps and expands optimization scope.
XR_FUNC XmPassChange xm_pass_merge_blocks(XmFunc *func);

// Store-to-Load Forwarding: when a LOAD_FIELD reads from the same object+offset
// that a preceding STORE_FIELD just wrote, replace the load with a MOV from
// the stored value. Also eliminates redundant loads (load-load forwarding).
XR_FUNC XmPassChange xm_pass_store_to_load(XmFunc *func);

// Redundant Guard Elimination: eliminate duplicate GUARD_TAG/GUARD_NONNULL/
// GUARD_CLASS/GUARD_KLASS within each block. Useful after guard hoisting.
XR_FUNC XmPassChange xm_pass_elim_guards(XmFunc *func);

// Branch Value Propagation: when BR(cond) branches, propagate known values
// into single-predecessor successors. E.g., if cond = EQ(a, K) and the true
// successor has only one predecessor, replace uses of a with K in that block.
// Also propagates cond == 0 into the false successor.
XR_FUNC XmPassChange xm_pass_propjnz(XmFunc *func);

// If-Conversion: convert simple diamond CFG patterns into conditional select
// instructions (XM_SELECT_COND + XM_SELECT), eliminating branches.
// Pattern: BR(cond) → then/else → merge with PHI → SELECT + JMP merge
XR_FUNC XmPassChange xm_pass_ifconvert(XmFunc *func);

// TypePropagation: propagate precise XrType* through the IR.
// 1. GUARD_TAG narrows guarded vreg type in dominated blocks
// 2. Arithmetic result type inferred from operand types
// 3. Known CALL_KNOWN return types propagated to dst vreg
// Should run before select_rep for maximum type precision.
XR_FUNC XmPassChange xm_pass_type_prop(XmFunc *func);

// Type-driven specialization: lower polymorphic RT_* instructions to
// monomorphic native ops when type_prop has proven operand types.
// RT_LT/LE/EQ(I64,I64) → LT/LE/EQ, RT_LT(F64,F64) → FLT, etc.
// RT_ADD/SUB/MUL/DIV(F64,F64) → FADD/FSUB/FMUL/FDIV.
// Should run immediately after type_prop for maximum benefit.
XR_FUNC XmPassChange xm_pass_specialize(XmFunc *func);

// SelectRepresentations: eliminate redundant BOX/UNBOX pairs
XR_FUNC XmPassChange xm_pass_select_rep(XmFunc *func);

// InsertWriteBarriers: insert XM_BARRIER_FWD after XM_STORE_FIELD
XR_FUNC XmPassChange xm_insert_write_barriers(XmFunc *func);

// EliminateWriteBarriers: remove redundant BARRIER_FWD instructions.
// Rule 1: parent freshly allocated in same block (young → no barrier needed)
// Rule 2: duplicate barrier on same parent (already in remembered set)
// Both rules invalidated by GC-triggering instructions.
XR_FUNC XmPassChange xm_pass_elim_write_barriers(XmFunc *func);

// InsertArcReleases: AOT-only pass — insert XM_RELEASE before each RET for
// locally-allocated PTR vregs that are not returned.
// Only used in AOT transpile mode (not JIT).
XR_FUNC XmPassChange xm_insert_arc_releases(XmFunc *func);

// Escape Analysis + Scalar Replacement: replace non-escaping XM_ALLOC
// with virtual registers for each field (eliminates heap allocation).
// Per-block analysis: only optimizes allocations that don't escape the block.
XR_FUNC XmPassChange xm_pass_escape_analysis(XmFunc *func);

// Automatic Function Inlining: inline small XM_CALL_KNOWN callees.
// caller_proto needed for recursion detection and callee Xm build.
XR_FUNC XmPassChange xm_pass_auto_inline(XmFunc *func, XrProto *caller_proto);

// Canonicalize: normalize IR for better optimization opportunities.
// 1. Commutative ops: ensure constant on right side (enables more CSE matches)
// 2. Identity elimination: x+0→x, x*1→x, x&-1→x, x|0→x, x^0→x
// 3. Absorbing elimination: x*0→0, x&0→0
// 4. Double negation: NEG(NEG(x))→x, NOT(NOT(x))→x
// Run after SCCP and before CSE for maximum effect.
XR_FUNC XmPassChange xm_pass_canonicalize(XmFunc *func);

// Dead Store Elimination: remove stores that are overwritten before being read.
// Per-block analysis: tracks (obj, offset) pairs; a store is dead if a later
// store to the same (obj, offset) exists with no intervening load or GC point.
XR_FUNC XmPassChange xm_pass_dse(XmFunc *func);

// REDEFINE Insertion: insert XM_REDEFINE after GUARD instructions.
// Creates new SSA values with narrowed types for flow-sensitive type info.
// Rewrites dominated uses of the guarded vreg to the REDEFINE result.
XR_FUNC XmPassChange xm_pass_insert_redefines(XmFunc *func);

// Range Analysis: infer integer value ranges and eliminate redundant bounds checks.
// 1. Track loop induction variables: i=0; i<N; i++ → range [0, N-1]
// 2. Eliminate GUARD_BOUNDS where index range is provably within [0, length)
// 3. Propagate range constraints from GUARD_BOUNDS to dominated uses
XR_FUNC XmPassChange xm_pass_range_analysis(XmFunc *func);

/* ========== Function Inlining ========== */

// Inline callee into caller at a specific call site.
// call_block: block containing the call
// call_ins_idx: instruction index of the call within call_block
// callee: function to inline (must not be the same as caller)
// call_args: array of XmRef for callee parameters
// nargs: number of arguments
// Returns the XmRef of the inlined return value (or XM_NONE on failure).
XR_FUNC XmRef xm_inline_function(XmFunc *caller, XmBlock *call_block, uint32_t call_ins_idx,
                                 XmFunc *callee, XmRef *call_args, uint32_t nargs);

#endif  // XM_PASS_H
