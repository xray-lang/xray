/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_cell_access_core.h - Runtime-neutral capture-cell slot semantics.
 *
 * Dependency: the including header must define XrValue first.
 */

#ifndef XR_CELL_ACCESS_CORE_H
#define XR_CELL_ACCESS_CORE_H

#include "xr_cell_abi.h"
#include "xr_semantic_owner_ids_gen.h"
#include <stdint.h>

/* Backend adapters validate the carrier and release the replaced value. */
static inline XrValue xr_cell_access_load_core(const XrValue *slot) {
    return *slot;
}

static inline XrValue xr_cell_access_replace_core(XrValue *slot, XrValue value) {
    XrValue old = *slot;
    *slot = value;
    return old;
}

#define XR_CELL_ACCESS_OWNER_GUARD(owner_hi, owner_lo)                                            \
    ((void) sizeof(struct {                                                                        \
        unsigned int owner_id_must_be_shared_cell_access                                          \
            : (((uint64_t) (owner_hi) == XR_SEM_OWNER_ID_SHARED_CELL_ACCESS_HI &&                 \
                (uint64_t) (owner_lo) == XR_SEM_OWNER_ID_SHARED_CELL_ACCESS_LO)                    \
                   ? 1                                                                            \
                   : -1);                                                                         \
    }))

#define XR_CELL_ACCESS_CONSUMER_GUARD(consumer_bit)                                               \
    ((void) sizeof(struct {                                                                        \
        unsigned int consumer_must_be_declared_for_shared_cell_access                             \
            : (((uint32_t) (consumer_bit) != 0 &&                                                 \
                (((uint32_t) (consumer_bit) & ((uint32_t) (consumer_bit) - 1)) == 0) &&           \
                (XR_SEM_OWNER_ID_SHARED_CELL_ACCESS_CONSUMERS & (uint32_t) (consumer_bit)) != 0) \
                   ? 1                                                                            \
                   : -1);                                                                         \
    }))

#define XR_CELL_ACCESS_OWNER_APPLY(owner_hi, owner_lo, consumer_bit, expression)                  \
    (XR_CELL_ACCESS_OWNER_GUARD((owner_hi), (owner_lo)),                                          \
     XR_CELL_ACCESS_CONSUMER_GUARD((consumer_bit)), (expression))

#endif /* XR_CELL_ACCESS_CORE_H */
