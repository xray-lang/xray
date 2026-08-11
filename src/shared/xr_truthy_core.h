/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_truthy_core.h - Runtime-neutral truthiness rules.
 */

#ifndef XR_TRUTHY_CORE_H
#define XR_TRUTHY_CORE_H

#include "xr_semantic_owner_ids_gen.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum XrTruthyCoreKind {
    XR_TRUTHY_CORE_NULL = 0,
    XR_TRUTHY_CORE_BOOL,
    XR_TRUTHY_CORE_INT,
    XR_TRUTHY_CORE_FLOAT,
    XR_TRUTHY_CORE_SIZED,
    XR_TRUTHY_CORE_OBJECT,
} XrTruthyCoreKind;

static inline bool xr_truthy_core_eval_impl(XrTruthyCoreKind kind, int64_t i, double f,
                                            int64_t size) {
    switch (kind) {
        case XR_TRUTHY_CORE_NULL:
            return false;
        case XR_TRUTHY_CORE_BOOL:
        case XR_TRUTHY_CORE_INT:
            return i != 0;
        case XR_TRUTHY_CORE_FLOAT:
            return f != 0.0;
        case XR_TRUTHY_CORE_SIZED:
            return size != 0;
        case XR_TRUTHY_CORE_OBJECT:
            return true;
        default:
            return false;
    }
}

/* The semantic owner is part of every adapter call.  A bit-field width must be
 * an integer constant expression, so a runtime ID or a wrong owner ID fails at
 * compile time before it can create a second truthiness path. */
#define XR_TRUTHY_CORE_OWNER_GUARD(owner_hi, owner_lo)                                              \
    ((void) sizeof(struct {                                                                         \
        unsigned int owner_id_must_be_shared_truthiness                                             \
            : (((uint64_t) (owner_hi) == XR_SEM_OWNER_ID_SHARED_TRUTHINESS_HI &&                    \
                (uint64_t) (owner_lo) == XR_SEM_OWNER_ID_SHARED_TRUTHINESS_LO)                       \
                   ? 1                                                                              \
                   : -1);                                                                           \
    }))

#define XR_TRUTHY_CORE_CONSUMER_GUARD(consumer_bit)                                                \
    ((void) sizeof(struct {                                                                         \
        unsigned int consumer_must_be_declared_for_shared_truthiness                               \
            : (((uint32_t) (consumer_bit) != 0 &&                                                   \
                (((uint32_t) (consumer_bit) & ((uint32_t) (consumer_bit) - 1)) == 0) &&             \
                (XR_SEM_OWNER_ID_SHARED_TRUTHINESS_CONSUMERS & (uint32_t) (consumer_bit)) != 0)     \
                   ? 1                                                                              \
                   : -1);                                                                           \
    }))

#define xr_truthy_core_eval(owner_hi, owner_lo, consumer_bit, kind, i, f, size)                    \
    (XR_TRUTHY_CORE_OWNER_GUARD((owner_hi), (owner_lo)),                                            \
     XR_TRUTHY_CORE_CONSUMER_GUARD((consumer_bit)),                                                 \
     xr_truthy_core_eval_impl((kind), (i), (f), (size)))

#endif /* XR_TRUTHY_CORE_H */
