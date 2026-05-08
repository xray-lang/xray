/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmap_methods.h - Map / WeakMap builtin method implementations.
 *
 * KEY POINTS:
 *   - Each method ultimately delegates to an extern xr_map_*
 *     primitive, so all methods stay XR_FUNC extern. Wrapping them
 *     `static inline` would not change AOT codegen.
 *   - WeakMap semantics:
 *       - set() validates that args[0] is a heap object and throws
 *         XR_ERR_INVALID_ARG_TYPE on contract violation.
 *       - keys / values / entries / clear / hasValue / iterator /
 *         entriesIterator are blocked on weak maps; they return
 *         XR_NOTFOUND from the method body so the dispatcher's
 *         shared "method not found" path produces the standard
 *         diagnostic.
 *       - iterator on a regular map returns null (preserves the
 *         legacy stub).
 */

#ifndef XMAP_METHODS_H
#define XMAP_METHODS_H

#include "../../base/xdefs.h"

#ifdef __cplusplus
extern "C" {
#endif

struct XrayIsolate;
XR_FUNC void xr_map_register_native_type(struct XrayIsolate *isolate);

#ifdef __cplusplus
}
#endif

#endif /* XMAP_METHODS_H */
