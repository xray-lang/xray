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

// Forward declaration
typedef struct XrProto XrProto;

/* ========== Optimization Levels ========== */

typedef enum {
    XIR_OPT_NONE  = 0,  // -O0: DCE only (Tier 1 JIT — fast compile)
    XIR_OPT_BASIC = 1,  // -O1: DCE + ConstProp + ConstFold
    XIR_OPT_FULL  = 2,  // -O2: full pipeline (Tier 2 JIT / AOT)
} XirOptLevel;

/* ========== Pipeline API ========== */

// Run the optimization pipeline on a function at the given level
XR_FUNC void xir_run_pipeline(XirFunc *func, XirOptLevel opt);

// Extended pipeline with caller proto (enables auto-inlining at -O2)
XR_FUNC void xir_run_pipeline_ex(XirFunc *func, XirOptLevel opt, XrProto *proto);

/* ========== Individual Pass API ========== */

// Dead Code Elimination: remove instructions whose results are unused
XR_FUNC void xir_pass_dce(XirFunc *func);

// Constant Propagation + Folding: propagate and fold constant expressions
XR_FUNC void xir_pass_const_prop(XirFunc *func);

// Common Subexpression Elimination: replace duplicate pure computations (local, per-block)
XR_FUNC void xir_pass_cse(XirFunc *func);

// Global Value Numbering: dominator-based cross-block CSE (replaces local CSE at -O2)
XR_FUNC void xir_pass_gvn(XirFunc *func);

// Loop Invariant Code Motion: hoist invariant computations out of loops
XR_FUNC void xir_pass_licm(XirFunc *func);

// Copy Propagation: replace uses of MOV dst with the original src,
// collapsing copy chains. Run before DCE to expose dead MOVs.
XR_FUNC void xir_pass_copy_prop(XirFunc *func);

// Branch Simplification: convert branches with known-constant conditions
// into unconditional jumps, and simplify trivial diamond patterns.
XR_FUNC void xir_pass_branch_simp(XirFunc *func);

// Unreachable Block Elimination: remove blocks with no predecessors
// (except the entry block). Run after branch simplification.
XR_FUNC void xir_pass_remove_unreachable(XirFunc *func);

// Phi Simplification: replace phi nodes where all args are the same value
// (or all non-self args are the same) with a MOV from that value.
XR_FUNC void xir_pass_phi_simp(XirFunc *func);

// Allocation Sinking: move XIR_ALLOC from dominating blocks into the
// specific branch where it's used, avoiding allocation on unused paths.
XR_FUNC void xir_pass_alloc_sink(XirFunc *func);

// Global Code Motion (GCM): Click's algorithm to move pure instructions
// to optimal positions — as early as dependencies allow, but placed in the
// block with the lowest loop depth between early and late bounds.
XR_FUNC void xir_pass_gcm(XirFunc *func);

// CFG Rebuild: reconstruct all predecessor lists from s1/s2 edges and
// remap phi args to match the new pred ordering. Call after any group of
// passes that modify CFG structure (branch_simp, remove_unreachable, merge_blocks).
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
XR_FUNC void xir_pass_split_critical_edges(XirFunc *func);

// Block Reordering: reorder func->blocks[] to minimize taken branches.
// Greedy fall-through chaining: BR false-branch as fall-through, loop bodies compact.
XR_FUNC void xir_pass_reorder_blocks(XirFunc *func);

// Block Merging: merge a block with its sole successor when the successor
// has exactly one predecessor. Reduces jumps and expands optimization scope.
XR_FUNC void xir_pass_merge_blocks(XirFunc *func);

// Store-to-Load Forwarding: when a LOAD_FIELD reads from the same object+offset
// that a preceding STORE_FIELD just wrote, replace the load with a MOV from
// the stored value. Also eliminates redundant loads (load-load forwarding).
XR_FUNC void xir_pass_store_to_load(XirFunc *func);

// Redundant Guard Elimination: eliminate duplicate GUARD_TAG/GUARD_SHAPE/
// GUARD_NONNULL/GUARD_CLASS within each block. Useful after guard hoisting.
XR_FUNC void xir_pass_elim_guards(XirFunc *func);

// Branch Value Propagation: when BR(cond) branches, propagate known values
// into single-predecessor successors. E.g., if cond = EQ(a, K) and the true
// successor has only one predecessor, replace uses of a with K in that block.
// Also propagates cond == 0 into the false successor.
XR_FUNC void xir_pass_propjnz(XirFunc *func);

// If-Conversion: convert simple diamond CFG patterns into conditional select
// instructions (XIR_SELECT_COND + XIR_SELECT), eliminating branches.
// Pattern: BR(cond) → then/else → merge with PHI → SELECT + JMP merge
XR_FUNC void xir_pass_ifconvert(XirFunc *func);

// TypePropagation: propagate precise XrType* through the IR.
// 1. GUARD_TAG narrows guarded vreg type in dominated blocks
// 2. Arithmetic result type inferred from operand types
// 3. Known CALL_KNOWN return types propagated to dst vreg
// Should run before select_rep for maximum type precision.
XR_FUNC void xir_pass_type_prop(XirFunc *func);

// Type-driven specialization: lower polymorphic RT_* instructions to
// monomorphic native ops when type_prop has proven operand types.
// RT_LT/LE/EQ(I64,I64) → LT/LE/EQ, RT_LT(F64,F64) → FLT, etc.
// RT_ADD/SUB/MUL/DIV(F64,F64) → FADD/FSUB/FMUL/FDIV.
// Should run immediately after type_prop for maximum benefit.
XR_FUNC void xir_pass_specialize(XirFunc *func);

// SelectRepresentations: eliminate redundant BOX/UNBOX pairs
XR_FUNC void xir_pass_select_rep(XirFunc *func);

// InsertWriteBarriers: insert XIR_BARRIER_FWD after XIR_STORE_FIELD
XR_FUNC void xir_insert_write_barriers(XirFunc *func);

// EliminateWriteBarriers: remove redundant BARRIER_FWD instructions.
// Rule 1: parent freshly allocated in same block (young → no barrier needed)
// Rule 2: duplicate barrier on same parent (already in remembered set)
// Both rules invalidated by GC-triggering instructions.
XR_FUNC void xir_pass_elim_write_barriers(XirFunc *func);

// InsertArcReleases: AOT-only pass — insert XIR_RELEASE before each RET for
// locally-allocated PTR vregs that are not returned.
// Only used in AOT transpile mode (not JIT).
XR_FUNC void xir_insert_arc_releases(XirFunc *func);

// Escape Analysis + Scalar Replacement: replace non-escaping XIR_ALLOC
// with virtual registers for each field (eliminates heap allocation).
// Per-block analysis: only optimizes allocations that don't escape the block.
XR_FUNC void xir_pass_escape_analysis(XirFunc *func);

// Automatic Function Inlining: inline small XIR_CALL_KNOWN callees.
// caller_proto needed for recursion detection and callee XIR build.
XR_FUNC void xir_pass_auto_inline(XirFunc *func, XrProto *caller_proto);

// Canonicalize: normalize IR for better optimization opportunities.
// 1. Commutative ops: ensure constant on right side (enables more CSE matches)
// 2. Identity elimination: x+0→x, x*1→x, x&-1→x, x|0→x, x^0→x
// 3. Absorbing elimination: x*0→0, x&0→0
// 4. Double negation: NEG(NEG(x))→x, NOT(NOT(x))→x
// Run after const_prop and before CSE for maximum effect.
XR_FUNC void xir_pass_canonicalize(XirFunc *func);

// Dead Store Elimination: remove stores that are overwritten before being read.
// Per-block analysis: tracks (obj, offset) pairs; a store is dead if a later
// store to the same (obj, offset) exists with no intervening load or GC point.
XR_FUNC void xir_pass_dse(XirFunc *func);

// REDEFINE Insertion: insert XIR_REDEFINE after GUARD instructions.
// Creates new SSA values with narrowed types for flow-sensitive type info.
// Rewrites dominated uses of the guarded vreg to the REDEFINE result.
XR_FUNC void xir_pass_insert_redefines(XirFunc *func);

// Range Analysis: infer integer value ranges and eliminate redundant bounds checks.
// 1. Track loop induction variables: i=0; i<N; i++ → range [0, N-1]
// 2. Eliminate GUARD_BOUNDS where index range is provably within [0, length)
// 3. Propagate range constraints from GUARD_BOUNDS to dominated uses
XR_FUNC void xir_pass_range_analysis(XirFunc *func);

/* ========== Function Inlining ========== */

// Inline callee into caller at a specific call site.
// call_block: block containing the call
// call_ins_idx: instruction index of the call within call_block
// callee: function to inline (must not be the same as caller)
// call_args: array of XirRef for callee parameters
// nargs: number of arguments
// Returns the XirRef of the inlined return value (or XIR_NONE on failure).
XR_FUNC XirRef xir_inline_function(XirFunc *caller, XirBlock *call_block,
                           uint32_t call_ins_idx, XirFunc *callee,
                           XirRef *call_args, uint32_t nargs);

#endif // XIR_PASS_H
