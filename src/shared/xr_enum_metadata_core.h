/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_enum_metadata_core.h - Runtime-neutral checked enum metadata views.
 */

#ifndef XR_ENUM_METADATA_CORE_H
#define XR_ENUM_METADATA_CORE_H

#include "xr_semantic_owner_ids_gen.h"
#include <stdint.h>

typedef enum XrEnumMetadataStatus {
    XR_ENUM_METADATA_OK = 0,
    XR_ENUM_METADATA_INDEX_OUT_OF_BOUNDS = 1
} XrEnumMetadataStatus;

typedef struct XrEnumMetadataResult {
    int64_t value;
    XrEnumMetadataStatus status;
} XrEnumMetadataResult;

static inline XrEnumMetadataResult xr_enum_metadata_variant_at_core(int64_t count,
                                                                    int64_t index) {
    XrEnumMetadataResult result = {0, XR_ENUM_METADATA_INDEX_OUT_OF_BOUNDS};
    if (index < 0 || index >= count)
        return result;
    result.value = index;
    result.status = XR_ENUM_METADATA_OK;
    return result;
}

static inline XrEnumMetadataResult xr_enum_metadata_payload_at_core(uint64_t view,
                                                                    int64_t index) {
    XrEnumMetadataResult result = {0, XR_ENUM_METADATA_INDEX_OUT_OF_BOUNDS};
    uint32_t ordinal = (uint32_t) view;
    uint32_t count = (uint32_t) (view >> 32);
    if (index < 0 || (uint64_t) index >= (uint64_t) count)
        return result;
    result.value = (int64_t) (((uint64_t) ordinal << 32) | (uint32_t) index);
    result.status = XR_ENUM_METADATA_OK;
    return result;
}

#define XR_ENUM_METADATA_ACCESS_OWNER_GUARD(owner_hi, owner_lo)                                  \
    ((void) sizeof(struct {                                                                        \
        unsigned int owner_id_must_be_shared_enum_metadata_access                                \
            : (((uint64_t) (owner_hi) == XR_SEM_OWNER_ID_SHARED_ENUM_METADATA_ACCESS_HI &&       \
                (uint64_t) (owner_lo) == XR_SEM_OWNER_ID_SHARED_ENUM_METADATA_ACCESS_LO)          \
                   ? 1                                                                            \
                   : -1);                                                                         \
    }))

#define XR_ENUM_METADATA_ACCESS_CONSUMER_GUARD(consumer_bit)                                     \
    ((void) sizeof(struct {                                                                        \
        unsigned int consumer_must_be_declared_for_shared_enum_metadata_access                   \
            : (((uint32_t) (consumer_bit) != 0 &&                                                 \
                (((uint32_t) (consumer_bit) & ((uint32_t) (consumer_bit) - 1)) == 0) &&           \
                (XR_SEM_OWNER_ID_SHARED_ENUM_METADATA_ACCESS_CONSUMERS &                          \
                 (uint32_t) (consumer_bit)) != 0)                                                 \
                   ? 1                                                                            \
                   : -1);                                                                         \
    }))

#define XR_ENUM_METADATA_ACCESS_OWNER_APPLY(owner_hi, owner_lo, consumer_bit, expression)         \
    (XR_ENUM_METADATA_ACCESS_OWNER_GUARD((owner_hi), (owner_lo)),                                 \
     XR_ENUM_METADATA_ACCESS_CONSUMER_GUARD((consumer_bit)), (expression))

#endif /* XR_ENUM_METADATA_CORE_H */
