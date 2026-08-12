/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_raw_memory_core.h - Runtime-neutral raw memory semantic owner.
 *
 * Positive copies rely on the unsafe caller's non-null and non-overlap proof.
 * A non-positive byte count is a no-op and does not inspect either pointer.
 */

#ifndef XR_RAW_MEMORY_CORE_H
#define XR_RAW_MEMORY_CORE_H

#if !defined(XR_RAW_MEMORY_C90)
#include "xr_semantic_owner_ids_gen.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#ifndef XR_RAW_MEMORY_INLINE
#define XR_RAW_MEMORY_INLINE static inline
#endif
#else
/* The restricted C90 runtime provides int64_t, uint*_t, size_t, and memcpy. */
#define XR_RAW_MEMORY_INLINE static
#endif

XR_RAW_MEMORY_INLINE void *xr_raw_memory_copy_nonoverlap(void *dst, const void *src,
                                                         int64_t count) {
    if (count <= 0)
        return dst;
    if (count <= 16) {
        uint8_t *dp = (uint8_t *) dst;
        const uint8_t *sp = (const uint8_t *) src;
        if (count >= 8) {
            uint64_t first = 0;
            memcpy(&first, sp, sizeof(first));
            memcpy(dp, &first, sizeof(first));
            if (count > 8) {
                uint64_t last = 0;
                memcpy(&last, sp + count - 8, sizeof(last));
                memcpy(dp + count - 8, &last, sizeof(last));
            }
            return dst;
        }
        if (count >= 4) {
            uint32_t first = 0;
            memcpy(&first, sp, sizeof(first));
            memcpy(dp, &first, sizeof(first));
            if (count > 4) {
                uint32_t last = 0;
                memcpy(&last, sp + count - 4, sizeof(last));
                memcpy(dp + count - 4, &last, sizeof(last));
            }
            return dst;
        }
        if (count >= 2) {
            uint16_t first = 0;
            memcpy(&first, sp, sizeof(first));
            memcpy(dp, &first, sizeof(first));
            if (count > 2)
                dp[2] = sp[2];
            return dst;
        }
        dp[0] = sp[0];
        return dst;
    }
    memcpy(dst, src, (size_t) count);
    return dst;
}

#if !defined(XR_RAW_MEMORY_C90)
#define XR_RAW_MEMORY_COPY_OWNER_GUARD(owner_hi, owner_lo)                                      \
    ((void) sizeof(struct {                                                                      \
        unsigned int owner_id_must_be_shared_raw_memory_copy                                    \
            : (((uint64_t) (owner_hi) == XR_SEM_OWNER_ID_SHARED_RAW_MEMORY_COPY_HI &&           \
                (uint64_t) (owner_lo) == XR_SEM_OWNER_ID_SHARED_RAW_MEMORY_COPY_LO)              \
                   ? 1                                                                          \
                   : -1);                                                                       \
    }))

#define XR_RAW_MEMORY_COPY_CONSUMER_GUARD(consumer_bit)                                         \
    ((void) sizeof(struct {                                                                      \
        unsigned int consumer_must_be_declared_for_shared_raw_memory_copy                       \
            : (((uint32_t) (consumer_bit) != 0 &&                                               \
                (((uint32_t) (consumer_bit) & ((uint32_t) (consumer_bit) - 1)) == 0) &&          \
                (XR_SEM_OWNER_ID_SHARED_RAW_MEMORY_COPY_CONSUMERS &                             \
                 (uint32_t) (consumer_bit)) != 0)                                               \
                   ? 1                                                                          \
                   : -1);                                                                       \
    }))

#define XR_RAW_MEMORY_COPY_OWNER_APPLY(owner_hi, owner_lo, consumer_bit, dst, src, count)       \
    (XR_RAW_MEMORY_COPY_OWNER_GUARD((owner_hi), (owner_lo)),                                     \
     XR_RAW_MEMORY_COPY_CONSUMER_GUARD((consumer_bit)),                                         \
     xr_raw_memory_copy_nonoverlap((dst), (src), (int64_t) (count)))
#endif

#undef XR_RAW_MEMORY_INLINE

#endif /* XR_RAW_MEMORY_CORE_H */
