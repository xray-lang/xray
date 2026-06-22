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
#include "../mem/xobj_header.h"
#include "../value/xvalue.h"
#include "../../shared/xr_cell_abi.h"

/* ========== XrCell: single-slot mutable capture cell (32 bytes) ========== */

/*
 * MEMORY LAYOUT:
 *   [XrObjHeader 16B][value 16B]
 *   Total = 32 bytes
 *
 * The post-header field set (XR_CELL_ABI_FIELDS) is shared with the AOT
 * runtime's xrt_cell_t so both backends keep an identical cell layout.
 */
typedef struct XrCell {
    XrObjHeader hdr;  // object header, type = XR_TCELL
    XR_CELL_ABI_FIELDS;
} XrCell;

#define XR_CELL_SIZE (sizeof(XrCell))

_Static_assert(sizeof(XrCell) == 32, "XrCell must be 32 bytes (16B header + 16B value)");

struct XrCoroutine;
struct XrVMRuntime;

// Allocate a new Cell on the coroutine Region heap.
XR_FUNC XrCell *xr_cell_new(struct XrVMRuntime *isolate, struct XrCoroutine *coro);

#endif  // XCELL_H
