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
    switch (op) {
        case XI_ARRAY_NEW:
        case XI_MAP_NEW:
        case XI_TUPLE_NEW:
        case XI_SET_NEW:
        case XI_JSON_NEW:
        case XI_CLOSURE_NEW:
        case XI_STR_CONCAT:
        case XI_REGEX_COMPILE:
            return true;
        default:
            return false;
    }
}

/* Join two escape levels (lattice meet = max). */
static inline XiEscapeLevel xi_esc_join(XiEscapeLevel a, XiEscapeLevel b) {
    return a > b ? a : b;
}

/* Run escape analysis on f and all its children (bottom-up).
 * Populates XiValue.escape for every value in the function tree.
 * Must be called after lowering (stage >= RAW). */
XR_FUNC void xi_escape_analyze(XiFunc *f);

#endif  // XI_ESCAPE_H
