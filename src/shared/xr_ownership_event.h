/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_ownership_event.h - Canonical logical ownership event alphabet
 *
 * KEY CONCEPT:
 *   Certificates and runtime audit lanes share one stable event namespace.
 *   Physical retain/release diagnostics are distinguished by event flags.
 */

#ifndef XR_OWNERSHIP_EVENT_H
#define XR_OWNERSHIP_EVENT_H

#include <stdint.h>

typedef enum XrOwnershipEventKind {
    XR_OWN_EVENT_ALLOC = 0,
    XR_OWN_EVENT_RETAIN = 1,
    XR_OWN_EVENT_RELEASE = 2,
    XR_OWN_EVENT_BORROW = 3,
    XR_OWN_EVENT_END_BORROW = 4,
    XR_OWN_EVENT_MOVE = 5,
    XR_OWN_EVENT_STORE = 6,
    XR_OWN_EVENT_PUBLISH = 7,
    XR_OWN_EVENT_DETACH = 8,
    XR_OWN_EVENT_SUSPEND = 9,
    XR_OWN_EVENT_RESUME = 10,
    XR_OWN_EVENT_CANCEL = 11,
    XR_OWN_EVENT_DESTROY = 12,
    XR_OWN_EVENT_PIN = 13,
    XR_OWN_EVENT_UNPIN = 14,
    XR_OWN_EVENT_RETURN = 15,
    XR_OWN_EVENT_COUNT = 16,
} XrOwnershipEventKind;

typedef enum XrOwnershipState {
    XR_OWN_UNINITIALIZED = 0,
    XR_OWN_OWNED_UNIQUE = 1,
    XR_OWN_OWNED_LOCAL = 2,
    XR_OWN_BORROWED = 3,
    XR_OWN_MOVED = 4,
    XR_OWN_PUBLISHED_SHARED = 5,
    XR_OWN_FRAME_OWNED = 6,
    XR_OWN_FOREIGN_OWNED = 7,
    XR_OWN_FOREIGN_BORROWED = 8,
    XR_OWN_RELEASED = 9,
    XR_OWN_IMMORTAL = 10,
    XR_OWN_STATE_COUNT = 11,
} XrOwnershipState;

typedef enum XrOwnershipProgramPoint {
    XR_OWN_POINT_AFTER_OPERATION = 0,
    XR_OWN_POINT_BLOCK_EXIT = 1,
    XR_OWN_POINT_EDGE = 2,
    XR_OWN_POINT_COUNT = 3,
} XrOwnershipProgramPoint;

#define XR_OWN_STATE_MASK(state) (UINT32_C(1) << (uint32_t) (state))
#define XR_OWN_STATE_MASK_ALL ((UINT32_C(1) << XR_OWN_STATE_COUNT) - UINT32_C(1))

#endif  // XR_OWNERSHIP_EVENT_H
