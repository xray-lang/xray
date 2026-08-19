/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_length_source_core.h - Which count a value's length is
 *
 * KEY CONCEPT:
 *   len(x) answers a different count per kind: elements for a sequence,
 *   entries for a keyed collection, and code points -- not bytes -- for a
 *   string. That mapping is the semantics; reading the count out of an object
 *   is representation, and each runtime does that its own way. Stating the
 *   mapping here keeps one backend from quietly switching a kind to a
 *   different count.
 */

#ifndef XR_LENGTH_SOURCE_CORE_H
#define XR_LENGTH_SOURCE_CORE_H

#include <stdint.h>

typedef enum {
    /* The value has no length; asking is a type error. */
    XR_LENGTH_SOURCE_NONE = 0,
    XR_LENGTH_SOURCE_ARRAY_ELEMENTS = 1,
    XR_LENGTH_SOURCE_SLICE_ELEMENTS = 2,
    XR_LENGTH_SOURCE_MAP_ENTRIES = 3,
    XR_LENGTH_SOURCE_SET_ENTRIES = 4,
    /* Code points, never bytes: len("日本語") is 3. A byte count is what
     * bytes() asks for and is deliberately a different question. */
    XR_LENGTH_SOURCE_STRING_RUNES = 5,
    XR_LENGTH_SOURCE_CHANNEL_BUFFERED = 6,
    XR_LENGTH_SOURCE_WORK_QUEUE_PENDING = 7,
    XR_LENGTH_SOURCE_RANGE_SPAN = 8,
} XrLengthSourceCore;

/* Whether this kind answers len() at all. */
static inline int xr_length_source_is_defined_core(XrLengthSourceCore source) {
    return source != XR_LENGTH_SOURCE_NONE;
}

/* Whether the count is over code points rather than storage units. Callers
 * that hold both a byte length and a rune length pick with this. */
static inline int xr_length_source_counts_runes_core(XrLengthSourceCore source) {
    return source == XR_LENGTH_SOURCE_STRING_RUNES;
}

#endif  // XR_LENGTH_SOURCE_CORE_H
