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
    bool inclusive_end;
} xrt_range_t;

static inline int64_t xrt_range_len_from_distance(uint64_t distance, uint64_t step,
                                                  bool inclusive_end) {
    if (step == 0)
        return 0;
    uint64_t base = distance / step;
    uint64_t extra = inclusive_end ? 1 : (distance % step != 0);
    if (base > UINT64_MAX - extra)
        return INT64_MAX;
    uint64_t len = base + extra;
    return len > (uint64_t) INT64_MAX ? INT64_MAX : (int64_t) len;
}

static inline int64_t xrt_range_length_ptr(const xrt_range_t *r) {
    if (!r || r->step == 0)
        return 0;
    if (r->step > 0) {
        if (r->inclusive_end ? (r->end < r->start) : (r->end <= r->start))
            return 0;
        return xrt_range_len_from_distance((uint64_t) r->end - (uint64_t) r->start,
                                           (uint64_t) r->step, r->inclusive_end);
    }
    if (r->inclusive_end ? (r->end > r->start) : (r->end >= r->start))
        return 0;
    int64_t neg_step = -r->step;
    return xrt_range_len_from_distance((uint64_t) r->start - (uint64_t) r->end, (uint64_t) neg_step,
                                       r->inclusive_end);
}

static inline bool xrt_range_contains_ptr(const xrt_range_t *r, int64_t value) {
    if (!r || r->step == 0)
        return false;
    if (r->step > 0) {
        if (value < r->start || (r->inclusive_end ? value > r->end : value >= r->end))
            return false;
        return ((uint64_t) value - (uint64_t) r->start) % (uint64_t) r->step == 0;
    }
    if (value > r->start || (r->inclusive_end ? value < r->end : value <= r->end))
        return false;
    return ((uint64_t) r->start - (uint64_t) value) % (uint64_t) (-r->step) == 0;
}

static inline int64_t xrt_range_index_ptr(const xrt_range_t *r, int64_t index, bool *ok) {
    int64_t len = xrt_range_length_ptr(r);
    // Subscript indexing does not support from-end negatives (matches array/string
    // `[i]` and the VM): an out-of-bounds index yields no value.
    if (ok)
        *ok = index >= 0 && index < len;
    if (index < 0 || index >= len)
        return 0;
    return r->start + index * r->step;
}

static inline int xrt_range_format_buf(const xrt_range_t *r, char *buf, size_t cap) {
    if (!r)
        return snprintf(buf, cap, "<Range>");
    const char *op = r->inclusive_end ? "..=" : "..";
    if (r->step == 1)
        return snprintf(buf, cap, "%" PRId64 "%s%" PRId64, r->start, op, r->end);
    return snprintf(buf, cap, "%" PRId64 "%s%" PRId64 ":%" PRId64, r->start, op, r->end, r->step);
}

static inline XrValue xrt_range_new_raw(int64_t start, int64_t end, int64_t step,
                                        bool inclusive_end) {
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
    r->inclusive_end = inclusive_end;
    return xr_mkptr(r, XR_TAG_RANGE);
}

static inline XrValue xrt_range_from_i64(int64_t start, int64_t end, bool inclusive_end) {
    return xrt_range_new_raw(start, end, 1, inclusive_end);
}

static inline XrValue xrt_range(XrValue start, XrValue end) {
    return xrt_range_from_i64(XR_TO_INT(start), XR_TO_INT(end), false);
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
