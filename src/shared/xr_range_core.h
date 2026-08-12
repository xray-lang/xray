/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_range_core.h - Runtime-neutral Range planning helpers.
 */

#ifndef XR_RANGE_CORE_H
#define XR_RANGE_CORE_H

#include "xr_semantic_owner_ids_gen.h"
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef struct XrRangeCore {
    int64_t start;
    int64_t end;
    int64_t step;
    bool inclusive_end;
} XrRangeCore;

enum {
    XR_RANGE_CORE_MATERIALIZE_MAX = 10000000
};

typedef enum XrRangeCoreMaterializeKind {
    XR_RANGE_CORE_MATERIALIZE_EMPTY = 0,
    XR_RANGE_CORE_MATERIALIZE_VALUES,
    XR_RANGE_CORE_MATERIALIZE_TOO_LARGE,
} XrRangeCoreMaterializeKind;

typedef struct XrRangeCoreMaterializePlan {
    XrRangeCoreMaterializeKind kind;
    int64_t length;
} XrRangeCoreMaterializePlan;

static inline XrRangeCore xr_range_core_make(int64_t start, int64_t end, int64_t step) {
    return (XrRangeCore) {start, end, step, false};
}

static inline XrRangeCore xr_range_core_make_with_bound(int64_t start, int64_t end, int64_t step,
                                                        bool inclusive_end) {
    return (XrRangeCore) {start, end, step, inclusive_end};
}

static inline uint64_t xr_range_core_abs_step_u64(int64_t step) {
    if (step >= 0)
        return (uint64_t) step;
    return (uint64_t) (-(step + 1)) + 1u;
}

static inline int64_t xr_range_core_count_from_distance(uint64_t distance, uint64_t step,
                                                        bool inclusive_end) {
    if (step == 0)
        return 0;
    uint64_t base = distance / step;
    uint64_t extra = inclusive_end ? 1u : (uint64_t) ((distance % step) != 0);
    if (base > UINT64_MAX - extra)
        return INT64_MAX;
    uint64_t count = base + extra;
    return count > (uint64_t) INT64_MAX ? INT64_MAX : (int64_t) count;
}

static inline int64_t xr_range_core_length(XrRangeCore r) {
    if (r.step == 0)
        return 0;
    if (r.step > 0) {
        if (r.inclusive_end ? (r.end < r.start) : (r.end <= r.start))
            return 0;
        return xr_range_core_count_from_distance((uint64_t) r.end - (uint64_t) r.start,
                                                 (uint64_t) r.step, r.inclusive_end);
    }
    if (r.inclusive_end ? (r.end > r.start) : (r.end >= r.start))
        return 0;
    return xr_range_core_count_from_distance((uint64_t) r.start - (uint64_t) r.end,
                                             xr_range_core_abs_step_u64(r.step), r.inclusive_end);
}

static inline bool xr_range_core_contains(XrRangeCore r, int64_t value) {
    if (r.step == 0)
        return false;
    if (r.step > 0) {
        if (value < r.start || (r.inclusive_end ? value > r.end : value >= r.end))
            return false;
        return (((uint64_t) value - (uint64_t) r.start) % (uint64_t) r.step) == 0;
    }
    if (value > r.start || (r.inclusive_end ? value < r.end : value <= r.end))
        return false;
    return (((uint64_t) r.start - (uint64_t) value) % xr_range_core_abs_step_u64(r.step)) == 0;
}

static inline int64_t xr_range_core_value_at(XrRangeCore r, int64_t index) {
    return (int64_t) ((uint64_t) r.start + (uint64_t) index * (uint64_t) r.step);
}

static inline int64_t xr_range_core_index(XrRangeCore r, int64_t index, bool *ok) {
    int64_t len = xr_range_core_length(r);
    bool in_bounds = index >= 0 && index < len;
    if (ok)
        *ok = in_bounds;
    if (!in_bounds)
        return 0;
    return xr_range_core_value_at(r, index);
}

static inline XrRangeCoreMaterializePlan xr_range_core_materialize_plan(XrRangeCore r) {
    int64_t len = xr_range_core_length(r);
    if (len <= 0)
        return (XrRangeCoreMaterializePlan) {XR_RANGE_CORE_MATERIALIZE_EMPTY, 0};
    if (len > XR_RANGE_CORE_MATERIALIZE_MAX)
        return (XrRangeCoreMaterializePlan) {XR_RANGE_CORE_MATERIALIZE_TOO_LARGE, len};
    return (XrRangeCoreMaterializePlan) {XR_RANGE_CORE_MATERIALIZE_VALUES, len};
}

static inline int xr_range_core_format_buf(XrRangeCore r, char *buf, size_t cap) {
    const char *op = r.inclusive_end ? "..=" : "..";
    if (r.step == 1)
        return snprintf(buf, cap, "%" PRId64 "%s%" PRId64, r.start, op, r.end);
    return snprintf(buf, cap, "%" PRId64 "%s%" PRId64 ":%" PRId64, r.start, op, r.end, r.step);
}

#define XR_RANGE_OWNER_GUARD(owner_hi, owner_lo)                                                   \
    ((void) sizeof(struct {                                                                        \
        unsigned int owner_id_must_be_shared_range                                                \
            : (((uint64_t) (owner_hi) == XR_SEM_OWNER_ID_SHARED_RANGE_HI &&                       \
                (uint64_t) (owner_lo) == XR_SEM_OWNER_ID_SHARED_RANGE_LO)                         \
                   ? 1                                                                            \
                   : -1);                                                                         \
    }))

#define XR_RANGE_CONSUMER_GUARD(consumer_bit)                                                     \
    ((void) sizeof(struct {                                                                        \
        unsigned int consumer_must_be_declared_for_shared_range                                   \
            : (((uint32_t) (consumer_bit) != 0 &&                                                 \
                (((uint32_t) (consumer_bit) & ((uint32_t) (consumer_bit) - 1)) == 0) &&           \
                (XR_SEM_OWNER_ID_SHARED_RANGE_CONSUMERS & (uint32_t) (consumer_bit)) != 0)        \
                   ? 1                                                                            \
                   : -1);                                                                         \
    }))

#define XR_RANGE_OWNER_APPLY(owner_hi, owner_lo, consumer_bit, start, end, inclusive_end)          \
    (XR_RANGE_OWNER_GUARD((owner_hi), (owner_lo)),                                                 \
     XR_RANGE_CONSUMER_GUARD((consumer_bit)),                                                      \
     xr_range_core_make_with_bound((int64_t) (start), (int64_t) (end), 1,                         \
                                   (bool) (inclusive_end)))

#endif  // XR_RANGE_CORE_H
