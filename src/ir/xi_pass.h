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
    bool cfg_changed;       /* blocks / edges / terminators altered */
    bool values_changed;    /* values added, removed, or replaced */
    bool types_changed;     /* type annotations refined */
    uint32_t n_removed;     /* values eliminated (for logging) */
    uint32_t n_added;       /* values inserted (for logging) */
} XiPassChange;

/* Sentinel: nothing changed */
static inline XiPassChange xi_pass_no_change(void) {
    return (XiPassChange){false, false, false, 0, 0};
}

/* Sentinel: everything may have changed (conservative) */
static inline XiPassChange xi_pass_change_all(void) {
    return (XiPassChange){true, true, true, 0, 0};
}

/* Merge two change records (logical OR of all fields, sum of counters) */
static inline XiPassChange xi_pass_merge(XiPassChange a, XiPassChange b) {
    return (XiPassChange){
        a.cfg_changed     || b.cfg_changed,
        a.values_changed  || b.values_changed,
        a.types_changed   || b.types_changed,
        a.n_removed + b.n_removed,
        a.n_added   + b.n_added,
    };
}

/* ========== Optimization Levels ========== */

typedef enum {
    XI_OPT_NONE  = 0,  /* no optimization (verify-only pipeline) */
    XI_OPT_LIGHT = 1,  /* constfold + strength_reduce + copy_prop + phi_simp + DCE */
    XI_OPT_FULL  = 2,  /* LIGHT + SCCP + GVN + LICM + GCM + inlining + if-conv */
} XiOptLevel;

/* ========== Pass Descriptor ========== */

/* Pass function signature: mutates XiFunc in-place, returns change record */
typedef XiPassChange (*XiPassFn)(XiFunc *f);

/* Flags describing per-pass properties */
#define XI_PASS_NONE        0u
#define XI_PASS_NEEDS_DOM   (1u << 0)   /* requires dominator tree */
#define XI_PASS_NEEDS_LOOP  (1u << 1)   /* requires loop detection */
#define XI_PASS_NEEDS_DEFUSE (1u << 2)  /* requires def-use chains */

typedef struct XiPassDesc {
    const char *name;       /* human-readable name for logging */
    XiPassFn fn;            /* pass entry point */
    XiOptLevel min_level;   /* minimum opt level to run this pass */
    uint32_t flags;         /* XI_PASS_* flags */
} XiPassDesc;

/* ========== Pipeline API ========== */

/* Run the optimization pipeline at the given level.
 * Executes passes in order, repeating until convergence or round cap.
 * Recurses into nested functions (children) at the same level.
 * Returns merged change record across all rounds and children. */
XR_FUNC XiPassChange xi_opt_run_pipeline(XiFunc *f, XiOptLevel level);

/* Maximum rounds before the fixed-point driver gives up.
 * Convergence is typically reached in 2-3 rounds. */
#define XI_OPT_MAX_ROUNDS 8

#endif // XI_PASS_H
