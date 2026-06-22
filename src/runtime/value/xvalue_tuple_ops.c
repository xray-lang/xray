/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvalue_tuple_ops.c - Tuple XrValue helpers.
 */

#include "xvalue.h"

XrValue xr_value_from_tuple(struct XrTuple *obj) {
    return XR_FROM_PTR(obj);
}

struct XrTuple *xr_value_to_tuple(XrValue v) {
    return xr_value_is_tuple(v) ? (struct XrTuple *) XR_TO_PTR(v) : NULL;
}
