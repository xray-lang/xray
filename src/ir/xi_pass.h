/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_pass.h - Xi IR optimization pass framework
 *
 * KEY CONCEPT:
 *   Declarative pass pipeline for Xi IR.  Each optimization is a
 *   self-contained function returning XiPassChange so the driver can:
 *     1. Detect convergence (stop when no pass mutates the IR)
 *     2. Re-run analyses only when their inputs changed
 *     3. Gate passes by optimization level
 *
 *   Three optimization levels control which passes fire:
 *     XI_OPT_NONE  — no pass runs; verification and the rest of the pipeline
 *                    still do
 *     XI_OPT_LIGHT — cheap cleanup (VM default: constfold, copy_prop, DCE)
 *     XI_OPT_FULL  — all passes including SCCP, GVN, LICM (AOT)
 *
 * INVARIANTS:
 *   1. Every pass preserves SSA form.
 *   2. Every pass preserves type annotations (value->type != NULL).
 *   3. Passes that modify CFG set cfg_changed = true.
 *   4. The driver re-verifies after each round in debug builds.
 */

#ifndef XI_PASS_H
#define XI_PASS_H

#include "xi.h"
#include "xi_evidence.h"
#include "../base/xdefs.h"
#include <stdbool.h>

/* ========== Pass Change Tracker ========== */

typedef struct XiPassChange {
    bool cfg_changed;    /* blocks / edges / terminators altered */
    bool values_changed; /* values added, removed, or replaced */
    bool types_changed;  /* type annotations refined */
    uint32_t n_removed;  /* values eliminated (for logging) */
    uint32_t n_added;    /* values inserted (for logging) */
} XiPassChange;

typedef struct XiRevisionDelta {
    bool ir_changed;
    bool cfg_changed;
    bool memory_changed;
    bool call_changed;
} XiRevisionDelta;

/* The driver completes this outcome from the pass report plus an audited IR
 * edit session. Evidence policy is therefore data, not an implicit side effect
 * hidden inside an optimization. */
typedef struct XiPassOutcome {
    XiPassChange change;
    XiEvidenceDomainMask invalidates;
    XiEvidenceDomainMask preserves;
    XiRevisionDelta revision_delta;
} XiPassOutcome;

/* Sentinel: nothing changed */
static inline XiPassChange xi_pass_no_change(void) {
    return (XiPassChange) {false, false, false, 0, 0};
}

/* Sentinel: everything may have changed (conservative) */
static inline XiPassChange xi_pass_change_all(void) {
    return (XiPassChange) {true, true, true, 0, 0};
}

/* Merge two change records (logical OR of all fields, sum of counters) */
static inline XiPassChange xi_pass_merge(XiPassChange a, XiPassChange b) {
    return (XiPassChange) {
        a.cfg_changed || b.cfg_changed,
        a.values_changed || b.values_changed,
        a.types_changed || b.types_changed,
        a.n_removed + b.n_removed,
        a.n_added + b.n_added,
    };
}

/* ========== Optimization Levels ========== */

typedef enum {
    /* No pass runs at all. The stage verifiers and the rest of the pipeline
     * still run, so this is the setting a bisection starts from: whatever it
     * answers, no optimization produced it. */
    XI_OPT_NONE = 0,
    XI_OPT_LIGHT = 1, /* constfold + strength_reduce + copy_prop + phi_simp + DCE */
    XI_OPT_FULL = 2,  /* LIGHT + SCCP + GVN + LICM + GCM + inlining + if-conv */
} XiOptLevel;

/* Ordered identity list for every optimization pass.  The order MUST match
 * xi_pass_table in xi_opt.c; the table validator compares the two name by
 * name at startup so a bit can never silently address the wrong pass. */
#define XI_OPT_PASS_LIST(X)                                                                        \
    X(TBAA, "tbaa")                                                                                \
    X(CONSTFOLD, "constfold")                                                                      \
    X(STRENGTH_REDUCE, "strength_reduce")                                                          \
    X(COPY_PROP, "copy_prop")                                                                      \
    X(MARK_ONE_SHOT_AWAIT, "mark_one_shot_await")                                                  \
    X(PHI_SIMPLIFY, "phi_simplify")                                                                \
    X(DCE, "dce")                                                                                  \
    X(SCCP, "sccp")                                                                                \
    X(RANGE, "range")                                                                              \
    X(GVN, "gvn")                                                                                  \
    X(LOOP_ROTATE, "loop_rotate")                                                                  \
    X(LICM, "licm")                                                                                \
    X(IVSR, "ivsr")                                                                                \
    X(LOOP_UNROLL, "loop_unroll")                                                                  \
    X(LOOP_SPLIT, "loop_split")                                                                    \
    X(LOOP_INV_BRANCH, "loop_inv_branch")                                                          \
    X(INLINE, "inline")                                                                            \
    X(TAIL_CALL, "tail_call")                                                                      \
    X(IFCONV, "ifconv")                                                                            \
    X(JUMP_THREAD, "jump_thread")                                                                  \
    X(BLOCK_SIMPLIFY, "block_simplify")                                                            \
    X(CONST_FIXPOINT, "const_fixpoint")

typedef enum {
#define XI_OPT_PASS_ID_ENTRY(upper, lower) XI_OPT_PASS_##upper,
    XI_OPT_PASS_LIST(XI_OPT_PASS_ID_ENTRY)
#undef XI_OPT_PASS_ID_ENTRY
        XI_OPT_PASS_ID_COUNT
} XiOptPassId;

/* One bit per pass.  The driver consults the bit at the pass' table index, so
 * a mask is a complete description of which optimizations are switched off.
 * A mask reaches the driver only through a pipeline configuration, which takes
 * it from the session optimizer policy; nothing else can withhold a pass. */
typedef uint32_t XiOptDisableMask;

#define XI_OPT_DISABLE_NONE 0u
#define XI_OPT_DISABLE_BIT(pass_id) ((XiOptDisableMask) 1u << (unsigned) (pass_id))

#define XI_OPT_DISABLE_TBAA XI_OPT_DISABLE_BIT(XI_OPT_PASS_TBAA)
#define XI_OPT_DISABLE_CONSTFOLD XI_OPT_DISABLE_BIT(XI_OPT_PASS_CONSTFOLD)
#define XI_OPT_DISABLE_STRENGTH_REDUCE XI_OPT_DISABLE_BIT(XI_OPT_PASS_STRENGTH_REDUCE)
#define XI_OPT_DISABLE_COPY_PROP XI_OPT_DISABLE_BIT(XI_OPT_PASS_COPY_PROP)
#define XI_OPT_DISABLE_MARK_ONE_SHOT_AWAIT XI_OPT_DISABLE_BIT(XI_OPT_PASS_MARK_ONE_SHOT_AWAIT)
#define XI_OPT_DISABLE_PHI_SIMPLIFY XI_OPT_DISABLE_BIT(XI_OPT_PASS_PHI_SIMPLIFY)
#define XI_OPT_DISABLE_DCE XI_OPT_DISABLE_BIT(XI_OPT_PASS_DCE)
#define XI_OPT_DISABLE_SCCP XI_OPT_DISABLE_BIT(XI_OPT_PASS_SCCP)
#define XI_OPT_DISABLE_RANGE XI_OPT_DISABLE_BIT(XI_OPT_PASS_RANGE)
#define XI_OPT_DISABLE_GVN XI_OPT_DISABLE_BIT(XI_OPT_PASS_GVN)
#define XI_OPT_DISABLE_LOOP_ROTATE XI_OPT_DISABLE_BIT(XI_OPT_PASS_LOOP_ROTATE)
#define XI_OPT_DISABLE_LICM XI_OPT_DISABLE_BIT(XI_OPT_PASS_LICM)
#define XI_OPT_DISABLE_IVSR XI_OPT_DISABLE_BIT(XI_OPT_PASS_IVSR)
#define XI_OPT_DISABLE_LOOP_UNROLL XI_OPT_DISABLE_BIT(XI_OPT_PASS_LOOP_UNROLL)
#define XI_OPT_DISABLE_LOOP_SPLIT XI_OPT_DISABLE_BIT(XI_OPT_PASS_LOOP_SPLIT)
#define XI_OPT_DISABLE_LOOP_INV_BRANCH XI_OPT_DISABLE_BIT(XI_OPT_PASS_LOOP_INV_BRANCH)
#define XI_OPT_DISABLE_INLINE XI_OPT_DISABLE_BIT(XI_OPT_PASS_INLINE)
#define XI_OPT_DISABLE_TAIL_CALL XI_OPT_DISABLE_BIT(XI_OPT_PASS_TAIL_CALL)
#define XI_OPT_DISABLE_IFCONV XI_OPT_DISABLE_BIT(XI_OPT_PASS_IFCONV)
#define XI_OPT_DISABLE_JUMP_THREAD XI_OPT_DISABLE_BIT(XI_OPT_PASS_JUMP_THREAD)
#define XI_OPT_DISABLE_BLOCK_SIMPLIFY XI_OPT_DISABLE_BIT(XI_OPT_PASS_BLOCK_SIMPLIFY)
#define XI_OPT_DISABLE_CONST_FIXPOINT XI_OPT_DISABLE_BIT(XI_OPT_PASS_CONST_FIXPOINT)

/* ========== Pass Descriptor ========== */

/* Pass function signature: mutates XiFunc in-place, returns change record */
typedef XiPassChange (*XiPassFn)(XiFunc *f);

/* Signature of a pass that runs other passes inside itself. XiPassFn cannot
 * describe such a pass: the mask that withholds its constituents never
 * reaches it, so every constituent would run whatever the caller asked for,
 * and a measurement taken with one of them named would report the wrong
 * pass' contribution. A composite pass publishes this entry point instead
 * and receives the same mask the driver applies to the pass table. */
typedef XiPassChange (*XiPassMaskedFn)(XiFunc *f, XiOptDisableMask disabled);

/* True when `disabled` withholds the pass at `pass_id`. Required passes are
 * structural and are never withheld. This is the single predicate that
 * decides it, so a composite pass reaches exactly the same answer as the
 * driver's walk over the pass table. Out-of-range ids are not withheld. */
XR_FUNC bool xi_pass_withheld_by_mask(XiOptDisableMask disabled, int pass_id);

/* True when the pass at `pass_id` carries XI_PASS_REQUIRED, so no mask can
 * withhold it. The spec parser asks before accepting a pass name, because a
 * spec that names a required pass is a request the compiler cannot honour.
 * Out-of-range ids are not required. */
XR_FUNC bool xi_pass_id_is_required(int pass_id);

/* Flags describing per-pass properties */
#define XI_PASS_NONE 0u
#define XI_PASS_NEEDS_DOM (1u << 0)    /* requires dominator tree */
#define XI_PASS_NEEDS_LOOP (1u << 1)   /* requires loop detection */
#define XI_PASS_NEEDS_DEFUSE (1u << 2) /* requires def-use chains */
#define XI_PASS_REQUIRED (1u << 3)     /* cannot be disabled by env / config */
#define XI_PASS_CORO_PLAN_SAFE (1u << 4) /* preserves frozen coroutine CFG anchors */
/* The pass does not maintain XiPassChange.n_removed / n_added. Statistics
 * print "n/a" for it instead of a zero, because a zero from a pass that
 * never counts is indistinguishable from a pass that changed nothing, and a
 * reader has drawn the wrong conclusion from exactly that. A rewrite that
 * moves values rather than adding or deleting them (LICM) or deletes edges
 * rather than values (loop splitting) has no honest number to print here,
 * so this flag states that rather than inventing one. The driver rejects a
 * pass that carries the flag and returns a non-zero count, so the flag
 * cannot quietly go stale when a pass starts counting. */
#define XI_PASS_NO_VALUE_COUNTS (1u << 5)

typedef struct XiPassDesc {
    const char *name;     /* human-readable name for logging */
    XiPassFn fn;          /* pass entry point */
    /* Entry point for a pass that re-runs other passes. Exactly one of `fn`
     * and `fn_masked` is set; the pass-table validator checks that. */
    XiPassMaskedFn fn_masked;
    XiOptLevel min_level; /* minimum opt level to run this pass */
    uint32_t flags;       /* XI_PASS_* flags */

    /* Legal stage window. Passes never advance stage; only consuming
     * transition APIs can do that. */
    XiStage min_stage;
    XiStage max_stage;

    /* Analysis invariant contract: required bits must already be present
     * before the pass runs; produced bits are marked present after it
     * returns successfully. */
    XiInvariantMask requires_inv_mask;
    XiInvariantMask produces_inv_mask;

    /* Revision-bound analysis contract. Required domains are obtained through
     * the analysis manager. Any rewrite invalidates every domain not listed in
     * preserves_evidence, plus the explicitly invalidated domains. */
    XiEvidenceDomainMask requires_evidence;
    XiEvidenceDomainMask produces_evidence;
    XiEvidenceDomainMask invalidates_evidence;
    XiEvidenceDomainMask preserves_evidence;
} XiPassDesc;

/* ========== Per-Pass Statistics ========== */

/* Maximum number of distinct passes tracked in a single pipeline run. */
#define XI_MAX_PASS_STATS 48

typedef struct XiPassStats {
    const char *name;     /* pass name (from XiPassDesc) */
    uint32_t invocations; /* how many times this pass was called */
    uint32_t n_removed;   /* total values eliminated */
    uint32_t n_added;     /* total values inserted */
    uint64_t elapsed_ns;  /* cumulative wall-clock nanoseconds */
    /* False when the pass carries XI_PASS_NO_VALUE_COUNTS: n_removed and
     * n_added are then not a measurement of it and say nothing about whether
     * it rewrote anything. */
    bool counts_reported;
} XiPassStats;

/* Aggregate statistics for the entire pipeline execution. */
typedef struct XiPipelineStats {
    XiPassStats passes[XI_MAX_PASS_STATS];
    uint32_t npass;        /* number of distinct passes tracked */
    uint32_t total_rounds; /* fixed-point iterations completed */
    uint64_t total_ns;     /* wall-clock nanoseconds for the whole pipeline */

    /* CFG-analysis cache miss counts copied from XiFunc when the
     * pipeline finishes.  A low ratio of recomputes-to-pass-invocations
     * means xi_ensure_* served most analysis queries from cache. */
    uint32_t rpo_recomputes;
    uint32_t dom_recomputes;
    uint32_t loop_recomputes;
} XiPipelineStats;

/* Optimization is a fallible compiler phase.  Invariant failures are carried
 * to the pipeline as data; they are never rendered or converted to aborts by
 * the pass driver. */
typedef struct XiOptResult {
    XiPassChange change;
    bool ok;
    const char *pass_name; /* static pass-table name, NULL for round/tree checks */
    int32_t round;
    char detail[512];
} XiOptResult;

/* ========== Pipeline API ========== */

/* Run the optimization pipeline at the given level.
 * Executes passes in order, repeating until convergence or round cap.
 * Recurses into nested functions (children) at the same level.
 * Returns merged change record across all rounds and children. */
XR_FUNC XiOptResult xi_opt_run_pipeline(XiFunc *f, XiOptLevel level);

/* Extended pipeline driver with per-pass statistics and optional time budget.
 *   stats     — if non-NULL, filled with per-pass timing and counters.
 *   budget_ns — if > 0, pipeline aborts early when cumulative wall-clock
 *               time exceeds this limit (0 = no limit). */
XR_FUNC XiOptResult xi_opt_run_pipeline_ex(XiFunc *f, XiOptLevel level, XiPipelineStats *stats,
                                           uint64_t budget_ns);
XR_FUNC XiOptResult xi_opt_run_pipeline_ex_with_mask(XiFunc *f, XiOptLevel level,
                                                     XiPipelineStats *stats, uint64_t budget_ns,
                                                     XiOptDisableMask disabled_passes);

/* Dump pipeline stats to stderr (human-readable, one line per pass). */
XR_FUNC void xi_pipeline_stats_dump(const XiPipelineStats *stats, const char *func_name);

/* Maximum rounds before the fixed-point driver gives up.
 * Convergence is typically reached in 2-3 rounds. */
#define XI_OPT_MAX_ROUNDS 8

/* ========== Pass Order Constraints ========== */

/* Declarative constraint: pass 'before' must run before pass 'after'.
 * The pipeline validates all constraints at startup and aborts if the
 * pass table violates any ordering requirement. */
typedef struct XiPassOrderConstraint {
    const char *before; /* name of the pass that must run first */
    const char *after;  /* name of the pass that must run later */
    const char *reason; /* human-readable justification */
} XiPassOrderConstraint;

/* Validate that the pass table ordering satisfies all constraints.
 * Returns true if valid.  On violation, writes a diagnostic to stderr
 * and returns false. Called once at pipeline startup. */
XR_FUNC bool xi_pass_order_check(void);

#endif  // XI_PASS_H
