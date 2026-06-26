/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrt_range.h - AOT lazy Range value
 */

#ifndef XRT_RANGE_H
#define XRT_RANGE_H

#include "xrt_arc.h"
#include "../shared/xr_range_core.h"
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int64_t start;
    int64_t end;
    int64_t step;
} xrt_range_t;

static inline int64_t xrt_range_length_ptr(const xrt_range_t *r) {
    if (!r)
        return 0;
    return xr_range_core_length(xr_range_core_make(r->start, r->end, r->step));
}

static inline bool xrt_range_contains_ptr(const xrt_range_t *r, int64_t value) {
    if (!r)
        return false;
    return xr_range_core_contains(xr_range_core_make(r->start, r->end, r->step), value);
}

static inline int64_t xrt_range_index_ptr(const xrt_range_t *r, int64_t index, bool *ok) {
    if (!r) {
        if (ok)
            *ok = false;
        return 0;
    }
    return xr_range_core_index(xr_range_core_make(r->start, r->end, r->step), index, ok);
}

static inline int xrt_range_format_buf(const xrt_range_t *r, char *buf, size_t cap) {
    if (!r)
        return snprintf(buf, cap, "<Range>");
    return xr_range_core_format_buf(xr_range_core_make(r->start, r->end, r->step), buf, cap);
}

static inline XrValue xrt_range_new_raw(int64_t start, int64_t end, int64_t step) {
    if (step == 0) {
        fprintf(stderr, "xrt_range: step must not be zero\n");
        abort();
    }
    xrt_range_t *r = (xrt_range_t *) XRT_MALLOC(sizeof(xrt_range_t));
    if (XR_UNLIKELY(!r)) {
        fprintf(stderr, "xrt_range: out of memory\n");
        abort();
    }
    r->start = start;
    r->end = end;
    r->step = step;
    return xr_mkptr(r, XR_TAG_RANGE);
}

static inline XrValue xrt_range_from_i64(int64_t start, int64_t end) {
    return xrt_range_new_raw(start, end, 1);
}

static inline XrValue xrt_range(XrValue start, XrValue end) {
    return xrt_range_from_i64(XR_TO_INT(start), XR_TO_INT(end));
}

static inline XrValue xrt_range_to_string(XrValue value) {
    XRT_STR_LIT_DEF(xs_range_fallback, "<Range>");
    if (value.tag != XR_TAG_RANGE || !value.ptr)
        return xr_str_lit(&xs_range_fallback);
    char buf[96];
    int n = xrt_range_format_buf((const xrt_range_t *) value.ptr, buf, sizeof(buf));
    if (n < 0)
        return xr_str_lit(&xs_range_fallback);
    size_t len = (size_t) n;
    if (len >= sizeof(buf))
        len = sizeof(buf) - 1;
    XrValue out = xrt_str_alloc(len);
    memcpy(xr_str_buf(out), buf, len);
    return out;
}

#endif  // XRT_RANGE_H
