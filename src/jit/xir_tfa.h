/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_tfa.h - Type Flow Analysis for whole-program type inference
 *
 * KEY CONCEPT:
 *   Propagate concrete types across function boundaries using a worklist-
 *   driven fixed-point iteration. Each function has a "summary" that maps
 *   input types to output types. When a caller's argument types change,
 *   the callee is re-analyzed until all types stabilize.
 *
 * WHY THIS DESIGN:
 *   - Untyped parameters (slot_type == ANY) lose optimization opportunities
 *   - Runtime type feedback (profile) only works after warm-up
 *   - TFA provides static whole-program type info before first execution
 *   - Results feed into proto->param_types for parameter specialization
 *     and proto->return_type_info for return type optimization
 *
 * RELATED MODULES:
 *   - xr_type.h: static type system (XrType with bit flags)
 *   - xslot_type.h: runtime slot types (XrSlotType for JIT/AOT)
 *   - xir_builder.c: consumes param_types to set slot_rep/slot_tag
 */

#ifndef XIR_TFA_H
#define XIR_TFA_H

#include "../runtime/value/xchunk.h"
#include "../runtime/value/xslot_type.h"
#include "../runtime/value/xtype.h"
#include <stdint.h>
#include <stdbool.h>
#include "../base/xdefs.h"

/* ========== Type Lattice ========== */

/*
 * XrType*-based lattice for TFA (canonical pointer comparison):
 *
 *         xr_type_new_unknown() (top / unknown)
 *        /    |    \    \
 *     int  float  string  Array<int>  ...
 *        \    |    /    /
 *          NULL (bottom / unreachable)
 *
 * Join rules:
 *   NULL ∨ T = T               (BOTTOM join anything = that thing)
 *   T ∨ T = T                 (same canonical type)
 *   T ∨ T? = T?              (nullable widening)
 *   int ∨ float = any          (incompatible → top)
 *   Array<int> ∨ Array<string> = any
 */

// Join two XrType* lattice elements (least upper bound)
// NULL = BOTTOM, xr_type_new_unknown() = TOP
static inline XrType *tfa_join(XrType *a, XrType *b) {
    if (!a) return b;                      // BOTTOM ∨ X = X
    if (!b) return a;                      // X ∨ BOTTOM = X
    if (a == b) return a;                  // same canonical
    if (a == xr_type_new_unknown()) return a;      // TOP ∨ X = TOP
    if (b == xr_type_new_unknown()) return b;      // X ∨ TOP = TOP
    // Nullable widening: T ∨ T? = T?
    if (a->kind == b->kind && a->is_nullable != b->is_nullable) {
        return a->is_nullable ? a : b;     // return the nullable one
    }
    // Numeric promotion: int ∨ float = float
    if ((a->kind == XR_KIND_INT && b->kind == XR_KIND_FLOAT) ||
        (a->kind == XR_KIND_FLOAT && b->kind == XR_KIND_INT))
        return xr_type_new_float();
    // Incompatible → any
    return xr_type_new_unknown();
}

/* ========== Function Summary ========== */

#define TFA_MAX_PARAMS 16

// Per-function type summary: tracks inferred param/return types
typedef struct TfaCallSite TfaCallSite;

typedef struct TfaSummary {
    XrProto *proto; // owning function

    XrType *param_types[TFA_MAX_PARAMS]; // inferred parameter types (NULL=BOTTOM)
    XrType *return_type; // inferred return type (NULL=BOTTOM)
    uint8_t nparam; // number of parameters (capped)

    bool on_worklist; // currently queued for re-analysis
    bool stable; // types did not change in last iteration
    uint32_t iteration; // last analysis iteration

    TfaCallSite *call_sites; // linked list of call sites targeting this func
} TfaSummary;

/* ========== Call Site ========== */

// Represents one call site: caller calls callee with specific arg types
typedef struct TfaCallSite {
    TfaSummary *caller;
    TfaSummary *callee;
    XrType *arg_types[TFA_MAX_PARAMS];  // argument types at this call site
    uint8_t nargs;
    struct TfaCallSite *next;           // linked list per callee
} TfaCallSite;

/* ========== TFA State ========== */

#define TFA_MAX_ITERATIONS 20

// Initial capacities (grow as needed)
#define TFA_INIT_FUNCS 64
#define TFA_INIT_CALLS 256
#define TFA_INIT_HASH  128

#define TFA_MAX_MODULES 32  // max modules with independent TFA analysis

typedef struct TfaState {
    // Function summaries (dynamically allocated)
    TfaSummary *summaries;
    uint32_t nsummary;
    uint32_t summary_cap;

    // Hash table: proto pointer → summary index (0 = empty, idx+1 stored)
    uint32_t *hash_table;
    uint32_t hash_size;     // must be power of 2
    uint32_t hash_mask;     // hash_size - 1

    // Call sites (dynamically allocated)
    TfaCallSite *calls;
    uint32_t ncall;
    uint32_t call_cap;

    // Worklist for fixed-point iteration (dynamically allocated)
    TfaSummary **worklist;
    uint32_t worklist_cap;
    uint32_t worklist_head, worklist_tail;

    // Per-module tracking: root protos that have been analyzed.
    // Prevents redundant re-analysis within a single JIT lifetime
    // while allowing newly loaded modules to be analyzed on demand.
    XrProto *analyzed_roots[TFA_MAX_MODULES];
    uint32_t n_analyzed_roots;

    // Statistics
    uint32_t iterations;
    uint32_t types_refined;
} TfaState;

/* ========== API ========== */

// Initialize TFA state (allocates internal arrays)
XR_FUNC void tfa_init(TfaState *tfa);

// Free all memory held by TFA state
XR_FUNC void tfa_free(TfaState *tfa);

// Register a function for analysis (returns its summary)
XR_FUNC TfaSummary *tfa_register_func(TfaState *tfa, XrProto *proto);

// Look up summary for a proto (NULL if not registered)
XR_FUNC TfaSummary *tfa_lookup(TfaState *tfa, XrProto *proto);

// Record a call site: caller invokes callee with given arg types
XR_FUNC void tfa_add_call(TfaState *tfa, TfaSummary *caller, TfaSummary *callee,
                  XrType *const *arg_types, uint8_t nargs);

// Run fixed-point iteration until all types stabilize
XR_FUNC void tfa_solve(TfaState *tfa);

// Apply TFA results to protos: set param_types and return_type_info
XR_FUNC void tfa_apply_results(TfaState *tfa);

// Whole-module analysis: register all protos, scan bytecode for call sites,
// run fixed-point solver, and apply inferred types to protos.
// This is the main entry point — call once after compilation, before JIT.
XR_FUNC void tfa_analyze_module(TfaState *tfa, XrProto *main_proto);

// Walk up proto->enclosing chain to find module root.
static inline XrProto *tfa_find_root(XrProto *proto) {
    while (proto && proto->enclosing) proto = proto->enclosing;
    return proto;
}

// Check if a module root has already been analyzed.
static inline bool tfa_is_module_analyzed(TfaState *tfa, XrProto *root) {
    if (!tfa || !root) return false;
    for (uint32_t i = 0; i < tfa->n_analyzed_roots; i++) {
        if (tfa->analyzed_roots[i] == root) return true;
    }
    return false;
}

// Print TFA statistics (debug)
XR_FUNC void tfa_dump_stats(TfaState *tfa);

#endif // XIR_TFA_H
