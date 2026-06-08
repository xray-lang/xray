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
    if (!r || r->step == 0)
        return 0;
    if (r->step > 0) {
        if (r->end <= r->start)
            return 0;
        return (r->end - r->start + r->step - 1) / r->step;
    }
    if (r->end >= r->start)
        return 0;
    int64_t neg_step = -r->step;
    return (r->start - r->end + neg_step - 1) / neg_step;
}

static inline bool xrt_range_contains_ptr(const xrt_range_t *r, int64_t value) {
    if (!r || r->step == 0)
        return false;
    if (r->step > 0) {
        if (value < r->start || value >= r->end)
            return false;
        return (value - r->start) % r->step == 0;
    }
    if (value > r->start || value <= r->end)
        return false;
    return (r->start - value) % (-r->step) == 0;
}

static inline int64_t xrt_range_index_ptr(const xrt_range_t *r, int64_t index, bool *ok) {
    int64_t len = xrt_range_length_ptr(r);
    if (index < 0)
        index += len;
    if (ok)
        *ok = index >= 0 && index < len;
    if (index < 0 || index >= len)
        return 0;
    return r->start + index * r->step;
}

static inline int xrt_range_format_buf(const xrt_range_t *r, char *buf, size_t cap) {
    if (!r)
        return snprintf(buf, cap, "<Range>");
    if (r->step == 1)
        return snprintf(buf, cap, "%" PRId64 "..%" PRId64, r->start, r->end);
    return snprintf(buf, cap, "%" PRId64 "..%" PRId64 ":%" PRId64, r->start, r->end, r->step);
}

static inline XrValue xrt_range_new_raw(int64_t start, int64_t end, int64_t step) {
    if (step == 0) {
        fprintf(stderr, "xrt_range: step must not be zero\n");
        abort();
    }
    xrt_range_t *r = (xrt_range_t *) XRT_MALLOC(sizeof(xrt_range_t));
    if (!r) {
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
    if (value.tag != XR_TAG_RANGE || !value.ptr)
        return xr_box_str("<Range>");
    char buf[96];
    int n = xrt_range_format_buf((const xrt_range_t *) value.ptr, buf, sizeof(buf));
    if (n < 0)
        return xr_box_str("<Range>");
    size_t len = (size_t) n;
    if (len >= sizeof(buf))
        len = sizeof(buf) - 1;
    XrValue out = xrt_str_alloc(len);
    memcpy(out.ptr, buf, len);
    ((char *) out.ptr)[len] = 0;
    return out;
}

#endif  // XRT_RANGE_H
