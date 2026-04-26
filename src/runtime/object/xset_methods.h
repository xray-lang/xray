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

#include "../value/xmethod_table.h"
#include "../symbol/xsymbol_table.h"

#ifdef __cplusplus
extern "C" {
#endif

XR_FUNC XrValue xr_set_method_has(XrayIsolate *iso, XrValue self,
                                  XrValue *args, int argc);
XR_FUNC XrValue xr_set_method_delete(XrayIsolate *iso, XrValue self,
                                     XrValue *args, int argc);
XR_FUNC XrValue xr_set_method_is_empty(XrayIsolate *iso, XrValue self,
                                       XrValue *args, int argc);
XR_FUNC XrValue xr_set_method_add(XrayIsolate *iso, XrValue self,
                                  XrValue *args, int argc);
XR_FUNC XrValue xr_set_method_clear(XrayIsolate *iso, XrValue self,
                                    XrValue *args, int argc);
XR_FUNC XrValue xr_set_method_union(XrayIsolate *iso, XrValue self,
                                    XrValue *args, int argc);
XR_FUNC XrValue xr_set_method_intersection(XrayIsolate *iso, XrValue self,
                                           XrValue *args, int argc);
XR_FUNC XrValue xr_set_method_difference(XrayIsolate *iso, XrValue self,
                                         XrValue *args, int argc);
XR_FUNC XrValue xr_set_method_symmetric_difference(XrayIsolate *iso,
                                                   XrValue self,
                                                   XrValue *args, int argc);
XR_FUNC XrValue xr_set_method_is_subset(XrayIsolate *iso, XrValue self,
                                        XrValue *args, int argc);
XR_FUNC XrValue xr_set_method_is_superset(XrayIsolate *iso, XrValue self,
                                          XrValue *args, int argc);
XR_FUNC XrValue xr_set_method_to_array(XrayIsolate *iso, XrValue self,
                                       XrValue *args, int argc);
XR_FUNC XrValue xr_set_method_iterator(XrayIsolate *iso, XrValue self,
                                       XrValue *args, int argc);
XR_FUNC XrValue xr_set_method_to_string(XrayIsolate *iso, XrValue self,
                                        XrValue *args, int argc);

extern const XrMethodSlot xr_set_method_table[SYMBOL_BUILTIN_COUNT];

#ifdef __cplusplus
}
#endif

#endif /* XSET_METHODS_H */
