/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_static_address_core.h - Runtime-neutral static-address contract.
 */

#ifndef XR_STATIC_ADDRESS_CORE_H
#define XR_STATIC_ADDRESS_CORE_H

#include "xr_semantic_owner_ids_gen.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum XrStaticAddressIdentity {
    XR_STATIC_ADDRESS_IDENTITY_INVALID = 0,
    XR_STATIC_ADDRESS_IDENTITY_MODULE,
    XR_STATIC_ADDRESS_IDENTITY_SYSTEM
} XrStaticAddressIdentity;

typedef struct XrStaticAddressPlan {
    XrStaticAddressIdentity identity;
    bool stable_escape;
    bool borrowed;
    bool requires_mutable_storage;
    bool requires_module_static_domain;
} XrStaticAddressPlan;

static inline XrStaticAddressPlan xr_static_address_plan_core(
    XrStaticAddressIdentity identity, bool want_mutable) {
    XrStaticAddressPlan plan = {identity, false, false, want_mutable, !want_mutable};
    if (identity == XR_STATIC_ADDRESS_IDENTITY_MODULE ||
        identity == XR_STATIC_ADDRESS_IDENTITY_SYSTEM) {
        plan.stable_escape = true;
        plan.borrowed = true;
    }
    return plan;
}

static inline bool xr_static_address_plan_is_exact_core(XrStaticAddressPlan plan) {
    if (plan.identity != XR_STATIC_ADDRESS_IDENTITY_MODULE &&
        plan.identity != XR_STATIC_ADDRESS_IDENTITY_SYSTEM)
        return false;
    return plan.stable_escape && plan.borrowed &&
           plan.requires_mutable_storage != plan.requires_module_static_domain;
}

#define XR_STATIC_ADDRESS_OWNER_GUARD(owner_hi, owner_lo)                                         \
    ((void) sizeof(struct {                                                                       \
        unsigned int owner_id_must_be_shared_static_address                                      \
            : (((uint64_t) (owner_hi) == XR_SEM_OWNER_ID_SHARED_STATIC_ADDRESS_HI &&             \
                (uint64_t) (owner_lo) == XR_SEM_OWNER_ID_SHARED_STATIC_ADDRESS_LO)               \
                   ? 1                                                                           \
                   : -1);                                                                        \
    }))

#define XR_STATIC_ADDRESS_CONSUMER_GUARD(consumer_bit)                                            \
    ((void) sizeof(struct {                                                                       \
        unsigned int consumer_must_be_declared_for_shared_static_address                         \
            : (((uint32_t) (consumer_bit) != 0 &&                                                \
                (((uint32_t) (consumer_bit) & ((uint32_t) (consumer_bit) - 1)) == 0) &&          \
                (XR_SEM_OWNER_ID_SHARED_STATIC_ADDRESS_CONSUMERS &                               \
                 (uint32_t) (consumer_bit)) != 0)                                                \
                   ? 1                                                                           \
                   : -1);                                                                        \
    }))

#define XR_STATIC_ADDRESS_OWNER_PLAN(owner_hi, owner_lo, consumer_bit, identity, want_mutable)   \
    (XR_STATIC_ADDRESS_OWNER_GUARD((owner_hi), (owner_lo)),                                       \
     XR_STATIC_ADDRESS_CONSUMER_GUARD((consumer_bit)),                                            \
     xr_static_address_plan_core((identity), (want_mutable)))

#endif /* XR_STATIC_ADDRESS_CORE_H */
