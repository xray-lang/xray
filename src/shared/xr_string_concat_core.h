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

#include "xr_semantic_owner_ids_gen.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Inline capacity: parts beyond this are emitted through the builder path,
 * and text longer than this allocates a scratch buffer. */
#define XR_STR_CONCAT_INLINE_PARTS 64
#define XR_STR_CONCAT_INLINE_BYTES 256

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

#define XR_STRING_CONCAT_OWNER_GUARD(owner_hi, owner_lo)                                           \
    ((void) sizeof(struct {                                                                        \
        unsigned int owner_id_must_be_shared_string_concat                                         \
            : (((uint64_t) (owner_hi) == XR_SEM_OWNER_ID_SHARED_STRING_CONCAT_HI &&                \
                (uint64_t) (owner_lo) == XR_SEM_OWNER_ID_SHARED_STRING_CONCAT_LO)                  \
                   ? 1                                                                             \
                   : -1);                                                                          \
    }))

#define XR_STRING_CONCAT_CONSUMER_GUARD(consumer_bit)                                              \
    ((void) sizeof(struct {                                                                        \
        unsigned int consumer_must_be_declared_for_shared_string_concat                            \
            : (((uint32_t) (consumer_bit) != 0 &&                                                  \
                (((uint32_t) (consumer_bit) & ((uint32_t) (consumer_bit) - 1)) == 0) &&            \
                (XR_SEM_OWNER_ID_SHARED_STRING_CONCAT_CONSUMERS & (uint32_t) (consumer_bit)) != 0) \
                   ? 1                                                                             \
                   : -1);                                                                          \
    }))

#endif  // XR_STRING_CONCAT_CORE_H
