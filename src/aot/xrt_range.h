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

#include "../shared/xr_range_core.h"
#include "xrt_arc.h"
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct xrt_range_s {
    int64_t start;
    int64_t end;
    int64_t step;
    bool inclusive_end;
} xrt_range_t;

static inline XrRangeCore xrt_range_core_view(const xrt_range_t *r) {
    return r ? xr_range_core_make_with_bound(r->start, r->end, r->step, r->inclusive_end)
             : xr_range_core_make(0, 0, 0);
}

static inline int64_t xrt_range_length_ptr(const xrt_range_t *r) {
    return xr_range_core_length(xrt_range_core_view(r));
}

static inline bool xrt_range_contains_ptr(const xrt_range_t *r, int64_t value) {
    return xr_range_core_contains(xrt_range_core_view(r), value);
}

static inline int64_t xrt_range_index_ptr(const xrt_range_t *r, int64_t index, bool *ok) {
    return xr_range_core_index(xrt_range_core_view(r), index, ok);
}

static inline int xrt_range_format_buf(const xrt_range_t *r, char *buf, size_t cap) {
    if (!r)
        return snprintf(buf, cap, "<Range>");
    return xr_range_core_format_buf(xrt_range_core_view(r), buf, cap);
}

static inline XrValue xrt_range_from_core(XrRangeCore core) {
    XR_ASSUME(core.step != 0);
    xrt_range_t *r = (xrt_range_t *) xrt_arc_alloc(sizeof(xrt_range_t));
    if (XR_UNLIKELY(!r)) {
        fprintf(stderr, "xrt_range: out of memory\n");
        abort();
    }
    r->start = core.start;
    r->end = core.end;
    r->step = core.step;
    r->inclusive_end = core.inclusive_end;
    return xr_mkptr(r, XR_TAG_RANGE);
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
