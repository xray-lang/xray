/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_string_concat_core.h - What concatenating string parts means
 *
 * KEY CONCEPT:
 *   Concatenation is two passes over the parts: sum their lengths, then copy
 *   them into one buffer. Stating it here keeps the two backends from each
 *   describing the shape of a part. Allocation and ownership stay with the
 *   caller, because they differ per runtime; the measurement and the copy do
 *   not.
 */

#ifndef XR_STRING_CONCAT_CORE_H
#define XR_STRING_CONCAT_CORE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* One part of a concatenation. An enum member renders as "Type.Member", which
 * is why a part carries two spans and a separator rather than one span. */
typedef struct {
    const char *a;
    const char *b;
    size_t alen;
    size_t blen;
    uint8_t joins_with_dot;
} XrStringConcatPartCore;

/* Total bytes the parts occupy, excluding the NUL. Returns SIZE_MAX when the
 * sum would wrap -- the caller reports it, since how to fail differs. */
static inline size_t xr_string_concat_total_core(const XrStringConcatPartCore *parts,
                                                 size_t count) {
    size_t total = 0;
    for (size_t i = 0; i < count; i++) {
        size_t len = parts[i].alen + (parts[i].joins_with_dot ? 1u + parts[i].blen : 0u);
        if (len > SIZE_MAX - total)
            return SIZE_MAX;
        total += len;
    }
    return total;
}

/* Copy one part and return the position after it. */
static inline char *xr_string_concat_copy_core(char *dst, const XrStringConcatPartCore *part) {
    memcpy(dst, part->a, part->alen);
    dst += part->alen;
    if (part->joins_with_dot) {
        *dst++ = '.';
        memcpy(dst, part->b, part->blen);
        dst += part->blen;
    }
    return dst;
}

/* Owner guards land with the observable-owner declaration, once both
 * adapters call through here. */

#endif  // XR_STRING_CONCAT_CORE_H
