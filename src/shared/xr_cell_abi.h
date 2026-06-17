/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_cell_abi.h - Shared capture-cell ABI (single mutable upvalue slot).
 *
 * Dependency: the including header must define XrValue first (same contract as
 * xr_map_set_abi.h). This keeps the field set usable from both the VM runtime
 * and the standalone AOT runtime without either including the other's value
 * header.
 *
 * A capture cell is one mutable slot wrapping a single captured variable. The
 * VM (XrCell) and the AOT runtime (xrt_cell_t) each prepend their own 16-byte
 * XrGCHeader and then carry an identical post-header field set, so both embed
 * XR_CELL_ABI_FIELDS to stay in lockstep — a cell crossing the coroutine
 * boundary needs no re-shelling.
 */

#ifndef XR_CELL_ABI_H
#define XR_CELL_ABI_H

#define XR_CELL_ABI_FIELDS XrValue value /* captured variable value */

typedef struct XrCellCore {
    XR_CELL_ABI_FIELDS;
} XrCellCore;

#endif  // XR_CELL_ABI_H
