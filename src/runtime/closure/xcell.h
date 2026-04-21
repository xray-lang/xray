/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcell.h - Closure capture cell (single mutable captured variable)
 *
 * KEY CONCEPT:
 *   XrCell is a 32-byte heap cell holding a single mutable captured
 *   variable. Each mutable local captured by a closure is wrapped in
 *   a cell; the cell pointer is stored in the closure's flat upvals[] array.
 *
 * LAYERING:
 *   Lives at the runtime closure layer (same as XrClosure / XrBoundMethod)
 *   so GC / deep-copy can traverse cells without reaching into vm/.
 */

#ifndef XCELL_H
#define XCELL_H

#include "../../base/xdefs.h"
#include "../gc/xgc_header.h"
#include "../value/xvalue.h"

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

struct XrCoroutine;
struct XrayIsolate;

// Allocate a new Cell on the coroutine Immix heap.
XR_FUNC XrCell *xr_cell_new(struct XrayIsolate *isolate,
                            struct XrCoroutine *coro);

#endif // XCELL_H
