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
 *     XI_OPT_NONE  — verification only (check pipelines, AOT dry-run)
 *     XI_OPT_LIGHT — cheap cleanup (VM default: constfold, copy_prop, DCE)
 *     XI_OPT_FULL  — all passes including SCCP, GVN, LICM (JIT Tier 2, AOT)
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
    XI_OPT_NONE = 0,  /* no optimization (verify-only pipeline) */
    XI_OPT_LIGHT = 1, /* constfold + strength_reduce + copy_prop + phi_simp + DCE */
    XI_OPT_FULL = 2,  /* LIGHT + SCCP + GVN + LICM + GCM + inlining + if-conv */
} XiOptLevel;

/* ========== Pass Descriptor ========== */

/* Pass function signature: mutates XiFunc in-place, returns change record */
typedef XiPassChange (*XiPassFn)(XiFunc *f);

/* Flags describing per-pass properties */
#define XI_PASS_NONE 0u
#define XI_PASS_NEEDS_DOM (1u << 0)    /* requires dominator tree */
#define XI_PASS_NEEDS_LOOP (1u << 1)   /* requires loop detection */
#define XI_PASS_NEEDS_DEFUSE (1u << 2) /* requires def-use chains */
#define XI_PASS_REQUIRED (1u << 3)     /* cannot be disabled by env / config */

typedef struct XiPassDesc {
    const char *name;     /* human-readable name for logging */
    XiPassFn fn;          /* pass entry point */
    XiOptLevel min_level; /* minimum opt level to run this pass */
    uint32_t flags;       /* XI_PASS_* flags */

    /* Stage contract: the pass requires func->stage >= input_stage.
     * On completion, func->stage is advanced to output_stage (if greater).
     * Most optimization passes are stage-preserving (input == output). */
    XiStage input_stage;  /* minimum stage required (0 = any) */
    XiStage output_stage; /* stage after this pass (0 = unchanged) */
} XiPassDesc;

/* ========== Per-Pass Statistics ========== */

/* Maximum number of distinct passes tracked in a single pipeline run. */
#define XI_MAX_PASS_STATS 16

typedef struct XiPassStats {
    const char *name;     /* pass name (from XiPassDesc) */
    uint32_t invocations; /* how many times this pass was called */
    uint32_t n_removed;   /* total values eliminated */
    uint32_t n_added;     /* total values inserted */
    uint64_t elapsed_ns;  /* cumulative wall-clock nanoseconds */
} XiPassStats;

/* Aggregate statistics for the entire pipeline execution. */
typedef struct XiPipelineStats {
    XiPassStats passes[XI_MAX_PASS_STATS];
    uint32_t npass;        /* number of distinct passes tracked */
    uint32_t total_rounds; /* fixed-point iterations completed */
    uint64_t total_ns;     /* wall-clock nanoseconds for the whole pipeline */
} XiPipelineStats;

/* ========== Pipeline API ========== */

/* Run the optimization pipeline at the given level.
 * Executes passes in order, repeating until convergence or round cap.
 * Recurses into nested functions (children) at the same level.
 * Returns merged change record across all rounds and children. */
XR_FUNC XiPassChange xi_opt_run_pipeline(XiFunc *f, XiOptLevel level);

/* Extended pipeline driver with per-pass statistics and optional time budget.
 *   stats     — if non-NULL, filled with per-pass timing and counters.
 *   budget_ns — if > 0, pipeline aborts early when cumulative wall-clock
 *               time exceeds this limit (0 = no limit). */
XR_FUNC XiPassChange xi_opt_run_pipeline_ex(XiFunc *f, XiOptLevel level, XiPipelineStats *stats,
                                            uint64_t budget_ns);

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
