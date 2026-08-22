/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_type_identity_core.h - Runtime-neutral public type identity rules.
 */

#ifndef XR_TYPE_IDENTITY_CORE_H
#define XR_TYPE_IDENTITY_CORE_H

#include "xr_semantic_owner_ids_gen.h"
#include <stdint.h>

/* These values are the public Type.* ids. Representation adapters select a
 * semantic kind; they do not repeat the observable numeric identity rule. */
typedef enum XrTypeIdentityCoreKind {
    XR_TYPE_IDENTITY_CORE_NULL = 0,
    XR_TYPE_IDENTITY_CORE_BOOL = 1,
    XR_TYPE_IDENTITY_CORE_I64 = 8,
    XR_TYPE_IDENTITY_CORE_F64 = 11,
    XR_TYPE_IDENTITY_CORE_STRING = 12,
    XR_TYPE_IDENTITY_CORE_FUNCTION = 13,
    XR_TYPE_IDENTITY_CORE_ARRAY = 14,
    XR_TYPE_IDENTITY_CORE_SET = 15,
    XR_TYPE_IDENTITY_CORE_MAP = 16,
    XR_TYPE_IDENTITY_CORE_INSTANCE = 17,
    XR_TYPE_IDENTITY_CORE_OBJECT = 18,
    XR_TYPE_IDENTITY_CORE_BIGINT = 19,
    XR_TYPE_IDENTITY_CORE_STRINGBUILDER = 20,
    XR_TYPE_IDENTITY_CORE_CHANNEL = 21,
    XR_TYPE_IDENTITY_CORE_REGEX = 22,
    XR_TYPE_IDENTITY_CORE_DATETIME = 23,
    XR_TYPE_IDENTITY_CORE_PANIC_INFO = 24,
    XR_TYPE_IDENTITY_CORE_ENUM_VALUE = 25,
    XR_TYPE_IDENTITY_CORE_ENUM_TYPE = 26,
    XR_TYPE_IDENTITY_CORE_BOUND_METHOD = 27,
    XR_TYPE_IDENTITY_CORE_ITERATOR = 28,
    XR_TYPE_IDENTITY_CORE_MODULE = 29,
    XR_TYPE_IDENTITY_CORE_COROUTINE = 30,
    XR_TYPE_IDENTITY_CORE_RANGE = 31,
    XR_TYPE_IDENTITY_CORE_TASK = 32,
    XR_TYPE_IDENTITY_CORE_NETCONN = 33,
    XR_TYPE_IDENTITY_CORE_NETLISTENER = 34,
    XR_TYPE_IDENTITY_CORE_ATOMIC = 35,
    XR_TYPE_IDENTITY_CORE_WORKQUEUE = 36,
    XR_TYPE_IDENTITY_CORE_RESULTGROUP = 37,
    XR_TYPE_IDENTITY_CORE_COUNTDOWNLATCH = 38,
    XR_TYPE_IDENTITY_CORE_SEMAPHORE = 39,
    XR_TYPE_IDENTITY_CORE_EVENTCOUNT = 40,
    XR_TYPE_IDENTITY_CORE_THREAD = 41,
    XR_TYPE_IDENTITY_CORE_BUFFER = 42,
    XR_TYPE_IDENTITY_CORE_RUNE = 43,
} XrTypeIdentityCoreKind;

static inline uint8_t xr_type_identity_core_eval_impl(XrTypeIdentityCoreKind kind) {
    switch (kind) {
        case XR_TYPE_IDENTITY_CORE_NULL:
        case XR_TYPE_IDENTITY_CORE_BOOL:
        case XR_TYPE_IDENTITY_CORE_I64:
        case XR_TYPE_IDENTITY_CORE_F64:
        case XR_TYPE_IDENTITY_CORE_STRING:
        case XR_TYPE_IDENTITY_CORE_FUNCTION:
        case XR_TYPE_IDENTITY_CORE_ARRAY:
        case XR_TYPE_IDENTITY_CORE_SET:
        case XR_TYPE_IDENTITY_CORE_MAP:
        case XR_TYPE_IDENTITY_CORE_INSTANCE:
        case XR_TYPE_IDENTITY_CORE_OBJECT:
        case XR_TYPE_IDENTITY_CORE_BIGINT:
        case XR_TYPE_IDENTITY_CORE_STRINGBUILDER:
        case XR_TYPE_IDENTITY_CORE_CHANNEL:
        case XR_TYPE_IDENTITY_CORE_REGEX:
        case XR_TYPE_IDENTITY_CORE_DATETIME:
        case XR_TYPE_IDENTITY_CORE_PANIC_INFO:
        case XR_TYPE_IDENTITY_CORE_ENUM_VALUE:
        case XR_TYPE_IDENTITY_CORE_ENUM_TYPE:
        case XR_TYPE_IDENTITY_CORE_BOUND_METHOD:
        case XR_TYPE_IDENTITY_CORE_ITERATOR:
        case XR_TYPE_IDENTITY_CORE_MODULE:
        case XR_TYPE_IDENTITY_CORE_COROUTINE:
        case XR_TYPE_IDENTITY_CORE_RANGE:
        case XR_TYPE_IDENTITY_CORE_TASK:
        case XR_TYPE_IDENTITY_CORE_NETCONN:
        case XR_TYPE_IDENTITY_CORE_NETLISTENER:
        case XR_TYPE_IDENTITY_CORE_ATOMIC:
        case XR_TYPE_IDENTITY_CORE_WORKQUEUE:
        case XR_TYPE_IDENTITY_CORE_RESULTGROUP:
        case XR_TYPE_IDENTITY_CORE_COUNTDOWNLATCH:
        case XR_TYPE_IDENTITY_CORE_SEMAPHORE:
        case XR_TYPE_IDENTITY_CORE_EVENTCOUNT:
        case XR_TYPE_IDENTITY_CORE_THREAD:
        case XR_TYPE_IDENTITY_CORE_BUFFER:
        case XR_TYPE_IDENTITY_CORE_RUNE:
            return (uint8_t) kind;
        default:
            return UINT8_MAX;
    }
}

/* Stable owner and consumer selection must remain integer constant
 * expressions. A surrogate ID, undeclared consumer, or runtime lookup fails
 * compilation instead of opening a fallback type-identity path. */
#define XR_TYPE_IDENTITY_CORE_OWNER_GUARD(owner_hi, owner_lo)                                    \
    ((void) sizeof(struct {                                                                       \
        unsigned int owner_id_must_be_primitive_type_identity                                    \
            : (((uint64_t) (owner_hi) == XR_SEM_OWNER_ID_PRIMITIVE_TYPE_IDENTITY_HI &&           \
                (uint64_t) (owner_lo) == XR_SEM_OWNER_ID_PRIMITIVE_TYPE_IDENTITY_LO)              \
                   ? 1                                                                            \
                   : -1);                                                                         \
    }))

#define XR_TYPE_IDENTITY_CORE_CONSUMER_GUARD(consumer_bit)                                      \
    ((void) sizeof(struct {                                                                       \
        unsigned int consumer_must_be_declared_for_primitive_type_identity                       \
            : (((uint32_t) (consumer_bit) != 0 &&                                                 \
                (((uint32_t) (consumer_bit) & ((uint32_t) (consumer_bit) - 1)) == 0) &&           \
                (XR_SEM_OWNER_ID_PRIMITIVE_TYPE_IDENTITY_CONSUMERS &                              \
                 (uint32_t) (consumer_bit)) != 0)                                                 \
                   ? 1                                                                            \
                   : -1);                                                                         \
    }))

#define xr_type_identity_core_eval(owner_hi, owner_lo, consumer_bit, kind)                       \
    (XR_TYPE_IDENTITY_CORE_OWNER_GUARD((owner_hi), (owner_lo)),                                  \
     XR_TYPE_IDENTITY_CORE_CONSUMER_GUARD((consumer_bit)),                                       \
     xr_type_identity_core_eval_impl((kind)))

#endif /* XR_TYPE_IDENTITY_CORE_H */
