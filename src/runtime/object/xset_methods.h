/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xset_methods.h - Set / WeakSet builtin method implementations.
 *
 * KEY POINTS:
 *   - Every method ultimately delegates to an extern xr_set_*
 *     primitive. Inlining a wrapper on top would not save anything
 *     real (one indirect call vs one direct call), so all methods
 *     stay XR_FUNC extern in xset_methods.c.
 *   - WeakSet semantics:
 *       - add() validates that args[0] is a heap object and throws
 *         a catchable XR_ERR_INVALID_ARG_TYPE on violation.
 *       - clear / union / intersection / difference /
 *         symmetric_difference / isSubset / isSuperset / toArray /
 *         iterator are blocked on weak sets — they return
 *         XR_NOTFOUND so the dispatcher reports "method not found".
 */

#ifndef XSET_METHODS_H
#define XSET_METHODS_H

#include "../../base/xdefs.h"

#ifdef __cplusplus
extern "C" {
#endif

struct XrayIsolate;
XR_FUNC void xr_set_register_native_type(struct XrayIsolate *isolate);

#ifdef __cplusplus
}
#endif

#endif /* XSET_METHODS_H */
