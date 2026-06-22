/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvalue_string_ops.c - String XrValue conversion helpers.
 */

#include "xvalue.h"
#include "../../base/xchecks.h"
#include "../object/xstring.h"

const char *xr_value_str_data(const XrValue *v) {
    XR_DCHECK(v != NULL, "value_str_data: NULL value");
    XR_DCHECK(v->ptr != NULL, "value_str_data: NULL ptr");
    return ((XrString *) v->ptr)->data;
}

uint32_t xr_value_str_len(const XrValue *v) {
    XR_DCHECK(v != NULL, "value_str_len: NULL value");
    XR_DCHECK(v->ptr != NULL, "value_str_len: NULL ptr");
    return ((XrString *) v->ptr)->length;
}

XrValue xr_string_value(XrString *str) {
    XR_DCHECK(str != NULL, "string_value: NULL string");
    return XR_FROM_STR(str);
}
