/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xbool_methods.h - Bool builtin method implementations.
 *
 * KEY POINTS:
 *   - bool currently exposes a single method: toString(). The method is
 *     hot, branchless, and side-effect-free, so it lives here as
 *     `static inline` — AOT-generated C #includes this header and
 *     inlines the call directly at the call site.
 *   - The inline definition is also used by xr_bool_register_native_type()
 *     to register the method on the XrClass dispatch table.
 */

#ifndef XBOOL_METHODS_H
#define XBOOL_METHODS_H

#include "xvalue.h"
#include "../object/xstring.h"
#include "../symbol/xsymbol_table.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * bool.toString() -> "true" | "false"
 *
 * Pure / no-GC / never throws. The returned XrValue holds a value
 * pointer to an interned XrString, so it is safe to keep across GC
 * safepoints.
 */
static inline XrValue xr_bool_to_string(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    (void) args;
    (void) argc;
    XR_DCHECK(iso != NULL, "xr_bool_to_string: NULL isolate");
    return XR_TO_BOOL(self) ? xr_string_value(xr_string_intern(iso, "true", 4, 0))
                            : xr_string_value(xr_string_intern(iso, "false", 5, 0));
}

struct XrayIsolate;
XR_FUNC void xr_bool_register_native_type(struct XrayIsolate *isolate);

#ifdef __cplusplus
}
#endif

#endif /* XBOOL_METHODS_H */
