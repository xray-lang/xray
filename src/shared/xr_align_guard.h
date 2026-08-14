/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_align_guard.h - Canonical guarded round-up to an alignment
 *
 * KEY CONCEPT:
 *   Rounding an offset up to an alignment is only meaningful when the
 *   alignment is a power of two, and zero is not one. A guard written as
 *   "alignment & (alignment - 1)" alone accepts zero, because zero minus one
 *   is an all-ones mask that passes the test and then rounds every value to
 *   zero. That is silent corruption of a layout rather than a refusal, so the
 *   rule is stated once here and both widths carry it.
 *
 *   The value guard is the same rule at the other end: a value close enough to
 *   the maximum that rounding it up would wrap is refused rather than folded.
 *
 *   Callers keep their own decision about what a refusal means; they only stop
 *   restating what makes an alignment usable.
 */

#ifndef XR_ALIGN_GUARD_H
#define XR_ALIGN_GUARD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static inline bool xr_align_is_usable_size(size_t alignment) {
    return alignment != 0u && (alignment & (alignment - 1u)) == 0u;
}

static inline bool xr_align_is_usable_u32(uint32_t alignment) {
    return alignment != 0u && (alignment & (alignment - 1u)) == 0u;
}

static inline bool xr_checked_align_size(size_t value, size_t alignment, size_t *out) {
    if (!out || !xr_align_is_usable_size(alignment))
        return false;
    size_t mask = alignment - 1u;
    if (value > SIZE_MAX - mask)
        return false;
    *out = (value + mask) & ~mask;
    return true;
}

static inline bool xr_checked_align_u32(uint32_t value, uint32_t alignment, uint32_t *out) {
    if (!out || !xr_align_is_usable_u32(alignment))
        return false;
    uint32_t mask = alignment - 1u;
    if (value > UINT32_MAX - mask)
        return false;
    *out = (value + mask) & ~mask;
    return true;
}

#endif  // XR_ALIGN_GUARD_H
