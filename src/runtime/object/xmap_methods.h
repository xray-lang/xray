/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmap_methods.h - Map builtin method implementations.
 *
 * KEY POINTS:
 *   - Each method ultimately delegates to an extern xr_map_*
 *     primitive, so all methods stay XR_FUNC extern. Wrapping them
 *     `static inline` would not change AOT codegen.
 *   - iterator on a regular map returns null (preserves the
 *     legacy stub).
 */

#ifndef XMAP_METHODS_H
#define XMAP_METHODS_H

#include "../../base/xdefs.h"

#ifdef __cplusplus
extern "C" {
#endif

struct XrVMRuntime;
XR_FUNC void xr_map_register_native_type(struct XrVMRuntime *isolate);

#ifdef __cplusplus
}
#endif

#endif /* XMAP_METHODS_H */
