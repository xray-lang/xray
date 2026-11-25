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

/* A private synchronous move between two nullable slots in one frame.
 * Begin resolves and locks both owner fields. End writes the already verified
 * presence byte along with the owner, or leaves both values alone if tag is
 * null. It always unlocks and transfers the retired owner to the caller.
 * No guest or Frame operation can intervene. */
typedef struct XrtPlaceStagedMove {
    XrtManagedSlotLocation destination, source;
} XrtPlaceStagedMove;
XR_FUNC XrtManagedStatus xrt_place_staged_move_begin(XrtManagedFrame *, size_t destination,
    size_t source, XrtPlaceStagedMove *empty);
XR_FUNC XrtManagedHeader *xrt_place_staged_move_end(XrtPlaceStagedMove *, uint8_t *tag, bool present);
#endif // XNATIVE_PLACE_H
