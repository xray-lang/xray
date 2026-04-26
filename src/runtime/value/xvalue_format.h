/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvalue_format.h - Value to string formatting (XrValue -> XrString)
 *
 * KEY CONCEPT:
 *   Single canonical runtime formatter: given an XrValue of any kind
 *   (primitive, container, class instance, builtin object), produce an
 *   XrString suitable for user output, toString() and string concatenation.
 *   Used by VM, JIT runtime helpers and xvalue_print fallbacks.
 *
 * LAYERING:
 *   Lives alongside xvalue_print in runtime/value. All type lookups go
 *   through runtime/object, runtime/class, coro and module headers —
 *   no dependency on vm/ or jit/.
 */

#ifndef XVALUE_FORMAT_H
#define XVALUE_FORMAT_H

#include "../../base/xdefs.h"
#include "xvalue.h"
#include "../xstrbuf.h"

struct XrayIsolate;

// Produce a newly allocated XrString representing `val`.
// Fast paths for primitives; containers/objects delegate to xr_value_to_strbuf.
XR_FUNC XrString *xr_value_to_string(struct XrayIsolate *isolate, XrValue val);

// Append `val`'s formatted form onto `sb`. `depth` tracks recursion to
// break cycles and truncate deep structures.
XR_FUNC void xr_value_to_strbuf(struct XrayIsolate *isolate, XrStrBuf *sb, XrValue val, int depth);

#endif  // XVALUE_FORMAT_H
