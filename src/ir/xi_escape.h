/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_escape.h - Escape analysis for Xi IR
 *
 * Determines which heap-allocating values can be stack-allocated and
 * which need ARC or GC management. The escape level is encoded in
 * XiValue.escape (2-bit field).
 *
 * Escape lattice (monotonically non-decreasing during analysis):
 *   NO_ESCAPE      – value does not escape its defining function.
 *                    Safe for stack allocation.
 *   ARG_ESCAPE     – escapes via return or out-parameter.
 *                    Caller must extend lifetime; ARC candidate.
 *   HEAP_ESCAPE    – stored to heap (object field, closure capture,
 *                    container element). Requires ARC or GC tracking.
 *   GLOBAL_ESCAPE  – stored to global, sent to another goroutine,
 *                    or address taken in unknown context. Full GC.
 *
 * Only heap-allocating ops matter (ARRAY_NEW, MAP_NEW, JSON_NEW,
 * CLOSURE_NEW, STR_CONCAT, etc.). Scalar values (int, float, bool)
 * are always stack-allocated by the tagged value representation and
 * do not participate in escape analysis.
 */

#ifndef XI_ESCAPE_H
#define XI_ESCAPE_H

#include "xi.h"
#include "xi_ops_gen.h"

/* Escape levels — stored in XiValue.escape (2 bits). */
typedef enum {
    XI_ESC_NONE = 0,   /* does not escape — stack-allocatable */
    XI_ESC_ARG = 1,    /* escapes via return / out-param */
    XI_ESC_HEAP = 2,   /* stored to heap object or captured by closure */
    XI_ESC_GLOBAL = 3, /* stored to global / sent cross-goroutine */
} XiEscapeLevel;

/* Check whether an op is a heap-allocating instruction.
 * Only these ops produce values that benefit from escape analysis. */
static inline bool xi_op_is_heap_alloc(uint16_t op) {
    return xi_generated_op_escape_alloc(op) == XI_GEN_ESCAPE_ALLOC_HEAP;
}

/* Query the escape level imposed on arguments at a use site. */
static inline XiEscapeLevel xi_op_use_escape_level(uint16_t op) {
    switch (xi_generated_op_escape_use(op)) {
        case XI_GEN_ESCAPE_USE_NONE:
            return XI_ESC_NONE;
        case XI_GEN_ESCAPE_USE_ARG:
            return XI_ESC_ARG;
        case XI_GEN_ESCAPE_USE_HEAP:
            return XI_ESC_HEAP;
        case XI_GEN_ESCAPE_USE_GLOBAL:
            return XI_ESC_GLOBAL;
        case XI_GEN_ESCAPE_USE__COUNT:
            break;
    }
    return XI_ESC_HEAP;
}

/* Join two escape levels (lattice meet = max). */
static inline XiEscapeLevel xi_esc_join(XiEscapeLevel a, XiEscapeLevel b) {
    return a > b ? a : b;
}

/* Run escape analysis on f and all its children (bottom-up).
 * Populates XiValue.escape for every value in the function tree.
 * Must be called after lowering (stage >= RAW). */
XR_FUNC void xi_escape_analyze(XiFunc *f);

/* ========== Cross-Function Escape Summary ========== */

/* Maximum parameter count for escape summary (beyond this, use conservative). */
#define XI_ESC_SUMMARY_MAX_PARAMS 16

/* Per-function escape summary: records the worst-case escape level
 * for each parameter and the return value.
 *
 * Produced by xi_escape_compute_summary() after intraprocedural escape
 * analysis.  Consumed by callers to avoid the conservative HEAP_ESCAPE
 * assumption for call arguments. */
typedef struct XiEscapeSummary {
    uint8_t param_escape[XI_ESC_SUMMARY_MAX_PARAMS]; /* XiEscapeLevel per param */
    uint8_t return_escape; /* escape level of values reachable from return */
    uint8_t nparams;       /* number of valid entries in param_escape */
    bool valid;            /* set true after successful computation */
} XiEscapeSummary;

/* Compute escape summary for f after escape analysis has run.
 * Stores result in summary (caller-allocated).
 * Returns true on success, false if f is NULL or has too many params. */
XR_FUNC bool xi_escape_compute_summary(const XiFunc *f, XiEscapeSummary *summary);

/* Query the escape level imposed on a call argument using the callee's
 * summary.  Returns HEAP_ESCAPE if no summary is available (conservative). */
static inline XiEscapeLevel xi_escape_summary_for_arg(const XiEscapeSummary *summary,
                                                      uint16_t arg_idx) {
    if (!summary || !summary->valid || arg_idx >= summary->nparams)
        return XI_ESC_HEAP;
    return (XiEscapeLevel) summary->param_escape[arg_idx];
}

#endif  // XI_ESCAPE_H
