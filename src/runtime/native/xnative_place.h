/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_place.h - Internal support for the public ownership transaction ABI
 *
 * KEY CONCEPT:
 *   A place borrows storage, not a reference or mutation capability. Every
 *   possible rejection precedes publication; retirement runs while both storage
 *   domains still reject reentrant ownership changes.
 */
#ifndef XNATIVE_PLACE_H
#define XNATIVE_PLACE_H
#include "xnative_managed_root.h"
#include "xray_native_place.h"
typedef XrtManagedSlotReservation XrtPlaceReplacement;
/* A caller already owns the binding's admitted mutation authority. Begin
 * consumes REPLACE guards and locks the binding and empty candidate domains
 * once, including when both fields share the same Root. The registered handle
 * is address-bound and cannot be copied or used after end. No callback may
 * throw or longjmp; all allocation, publication and retirement returns here. */
XR_FUNC XrtManagedStatus xrt_place_replacement_begin(XrtPlace, XrtManagedFrame *, size_t,
    XrtPlaceReplacement *empty);
XR_FUNC XrtManagedStatus xrt_place_replacement_publish(XrtPlaceReplacement *);
XR_FUNC XrtManagedStatus xrt_place_replacement_end(XrtPlaceReplacement *);
#endif // XNATIVE_PLACE_H
