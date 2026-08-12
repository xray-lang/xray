/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_null_test_core.h - Runtime-neutral null observation semantics.
 *
 * Tagged runtimes observe null through the stable zero tag. Native pointer,
 * RawPtr, and string storage observe null through the null pointer. Carrier
 * validation and representation selection remain mechanical backend work.
 */

#ifndef XR_NULL_TEST_CORE_H
#define XR_NULL_TEST_CORE_H

#if !defined(XR_NULL_TEST_C90)
#include "xr_semantic_owner_ids_gen.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define XR_NULL_TEST_INLINE static inline
#else
/* Restricted C90 provides bool, uint8_t, and NULL before including this core. */
#define XR_NULL_TEST_INLINE static
#endif

#define XR_NULL_TEST_TAG_NULL ((uint8_t) 0)

XR_NULL_TEST_INLINE bool xr_null_test_tagged_core(uint8_t tag) {
    return tag == XR_NULL_TEST_TAG_NULL;
}

XR_NULL_TEST_INLINE bool xr_null_test_pointer_is_null_core(const void *pointer) {
    return pointer == NULL;
}

#if !defined(XR_NULL_TEST_C90)
#define XR_NULL_TEST_OWNER_GUARD(owner_hi, owner_lo)                                              \
    ((void) sizeof(struct {                                                                        \
        unsigned int owner_id_must_be_shared_null_test                                            \
            : (((uint64_t) (owner_hi) == XR_SEM_OWNER_ID_SHARED_NULL_TEST_HI &&                  \
                (uint64_t) (owner_lo) == XR_SEM_OWNER_ID_SHARED_NULL_TEST_LO)                     \
                   ? 1                                                                            \
                   : -1);                                                                         \
    }))

#define XR_NULL_TEST_CONSUMER_GUARD(consumer_bit)                                                 \
    ((void) sizeof(struct {                                                                        \
        unsigned int consumer_must_be_declared_for_shared_null_test                               \
            : (((uint32_t) (consumer_bit) != 0 &&                                                 \
                (((uint32_t) (consumer_bit) & ((uint32_t) (consumer_bit) - 1)) == 0) &&           \
                (XR_SEM_OWNER_ID_SHARED_NULL_TEST_CONSUMERS & (uint32_t) (consumer_bit)) != 0)   \
                   ? 1                                                                            \
                   : -1);                                                                         \
    }))

#define XR_NULL_TEST_OWNER_APPLY(owner_hi, owner_lo, consumer_bit, expression)                    \
    (XR_NULL_TEST_OWNER_GUARD((owner_hi), (owner_lo)),                                            \
     XR_NULL_TEST_CONSUMER_GUARD((consumer_bit)), (expression))
#endif

#undef XR_NULL_TEST_INLINE

#endif /* XR_NULL_TEST_CORE_H */
