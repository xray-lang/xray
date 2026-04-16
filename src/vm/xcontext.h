/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcontext.h - Closure capture objects (Cell + legacy Context)
 *
 * KEY CONCEPT:
 *   XrCell is the primary capture object: a 32-byte heap cell holding
 *   a single mutable captured variable. Each mutable local captured by
 *   a closure is wrapped in a cell; the cell pointer is stored in the
 *   closure's flat upvals[] array.
 *
 *   XrContext is a legacy type kept only for GC traversal of existing
 *   heap objects (deep_copy). No new XrContext objects are created.
 */

#ifndef XCONTEXT_H
#define XCONTEXT_H

#include "../runtime/gc/xgc_header.h"
#include "../runtime/value/xvalue.h"

/* ========== Legacy Context Object (GC traversal only) ========== */

typedef struct XrContext {
    XrGCHeader gc;
    struct XrContext *parent;
    uint16_t slot_count;
    XrValue slots[];
} XrContext;

#define XR_CONTEXT_BASE_SIZE  (sizeof(XrContext))
#define XR_CONTEXT_SIZE(n)    (XR_CONTEXT_BASE_SIZE + (n) * sizeof(XrValue))

/* ========== XrCell: single-slot mutable capture cell (32 bytes) ========== */

/*
 * MEMORY LAYOUT:
 *   [XrGCHeader 16B][value 16B]
 *   Total = 32 bytes
 */
typedef struct XrCell {
    XrGCHeader gc;       // GC header, type = XR_TCELL
    XrValue value;       // captured variable value
} XrCell;

#define XR_CELL_SIZE  (sizeof(XrCell))

_Static_assert(sizeof(XrCell) == 32, "XrCell must be 32 bytes");

/* ========== API ========== */

struct XrCoroutine;
struct XrayIsolate;

// Allocate a new Cell on the coroutine Immix heap
XR_FUNC XrCell *xr_cell_new(struct XrayIsolate *isolate,
                              struct XrCoroutine *coro);

#endif // XCONTEXT_H
