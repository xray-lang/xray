/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_module_cell.h - Actual typed module Cell publication and borrowing
 */
#ifndef XNATIVE_MODULE_CELL_H
#define XNATIVE_MODULE_CELL_H
#include "xnative_module_state.h"
#include "xir/xxir_cell_owner_internal.h"

XR_FUNC XrXirValueStatus xrt_module_cell_construct(XrtModuleState *, uint32_t,
    const XrXirValue *, XrtModuleRef **);
XR_FUNC XrXirValueStatus xrt_module_cell_borrow(const XrtModuleRef *,
    XrXirValue *, XrXirCellPublication *, bool *);
#endif // XNATIVE_MODULE_CELL_H
