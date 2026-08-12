/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_data_pointer_core.h - Runtime-neutral borrowed data-pointer projection.
 *
 * Backend adapters validate and unpack their Array/Slice/fixed/static carrier.
 * This owner preserves the selected storage address and its lifetime class; it
 * does not add a null check, allocate storage, extend a borrow, or retain an
 * owner.  The caller remains responsible for the unsafe pointer proof.
 */

#ifndef XR_DATA_POINTER_CORE_H
#define XR_DATA_POINTER_CORE_H

#if !defined(XR_DATA_POINTER_C90)
#include "xr_semantic_owner_ids_gen.h"
#include <stdint.h>
#define XR_DATA_POINTER_INLINE static inline
#else
#define XR_DATA_POINTER_INLINE static
#endif

typedef enum XrDataPointerLifetime {
    XR_DATA_POINTER_OWNER_BORROW = 0,
    XR_DATA_POINTER_STATIC = 1
} XrDataPointerLifetime;

typedef struct XrDataPointerProjection {
    const void *address;
    XrDataPointerLifetime lifetime;
} XrDataPointerProjection;

XR_DATA_POINTER_INLINE XrDataPointerProjection
xr_data_pointer_project_core(const void *address, XrDataPointerLifetime lifetime) {
    XrDataPointerProjection result;
    result.address = address;
    result.lifetime = lifetime;
    return result;
}

#if !defined(XR_DATA_POINTER_C90)
#define XR_DATA_POINTER_OWNER_GUARD(owner_hi, owner_lo)                                         \
    ((void) sizeof(struct {                                                                      \
        unsigned int owner_id_must_be_shared_data_pointer                                      \
            : (((uint64_t) (owner_hi) == XR_SEM_OWNER_ID_SHARED_DATA_POINTER_HI &&              \
                (uint64_t) (owner_lo) == XR_SEM_OWNER_ID_SHARED_DATA_POINTER_LO)                 \
                   ? 1                                                                          \
                   : -1);                                                                       \
    }))

#define XR_DATA_POINTER_CONSUMER_GUARD(consumer_bit)                                            \
    ((void) sizeof(struct {                                                                      \
        unsigned int consumer_must_be_declared_for_shared_data_pointer                          \
            : (((uint32_t) (consumer_bit) != 0 &&                                               \
                (((uint32_t) (consumer_bit) & ((uint32_t) (consumer_bit) - 1)) == 0) &&          \
                (XR_SEM_OWNER_ID_SHARED_DATA_POINTER_CONSUMERS &                                \
                 (uint32_t) (consumer_bit)) != 0)                                               \
                   ? 1                                                                          \
                   : -1);                                                                       \
    }))

#define XR_DATA_POINTER_OWNER_APPLY(owner_hi, owner_lo, consumer_bit, address, lifetime)        \
    (XR_DATA_POINTER_OWNER_GUARD((owner_hi), (owner_lo)),                                        \
     XR_DATA_POINTER_CONSUMER_GUARD((consumer_bit)),                                             \
     xr_data_pointer_project_core((address), (lifetime)))
#endif

#undef XR_DATA_POINTER_INLINE

#endif /* XR_DATA_POINTER_CORE_H */
