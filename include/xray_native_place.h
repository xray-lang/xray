/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xray_native_place.h - Short ownership transactions over stable real owner fields
 *
 * KEY CONCEPT:
 *   A place borrows storage, not a reference or mutation capability. Every
 *   possible rejection precedes publication; retirement runs while both storage
 *   domains still reject reentrant ownership changes.
 */
#ifndef XRAY_NATIVE_PLACE_H
#define XRAY_NATIVE_PLACE_H
#include "xray_native_managed_root.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum XrtPlaceAction {
    XRT_PLACE_BORROW, XRT_PLACE_COPY_FROM, XRT_PLACE_MOVE_FROM, XRT_PLACE_REPLACE
} XrtPlaceAction;
typedef enum XrtPlaceNull {
    XRT_PLACE_NONNULL, XRT_PLACE_NULLABLE
} XrtPlaceNull;
/* This descriptor, its owner field, busy domain and guard context remain stable
 * through the synchronous call. Exactly one busy domain protects each field;
 * related fields may share it. These control objects have one execution owner;
 * plain bool is neither a cross-thread lock nor a source of mutation authority.
 * Preflight is mandatory and consumes the receiver's readiness/mutability facts.
 * Its domain is already busy; that fact is not a reason to reject this call.
 * It must not allocate, longjmp, mutate any place/descriptor/domain, or retire
 * these borrowed handles. It returns normally before any owner publication. */
typedef struct XrtExternalOwner {
    XrtManagedHeader **owner;
    bool *busy;
    XrtManagedStatus (*preflight)(void *, XrtPlaceAction);
    void *context;
} XrtExternalOwner;
typedef struct XrtPlace {
    XrtManagedFrame *frame;
    size_t slot;
    const XrtExternalOwner *external;
    XrtPlaceNull nulls;
} XrtPlace;

/* Select exactly one of frame or external. NONNULL rejects an empty source;
 * NULLABLE explicitly admits NULL as a typed value. An empty destination can
 * receive its first owner, but a NULL value requires a NULLABLE destination.
 * External guards also distinguish an initialized NULL from an unreadable field.
 * No Root infrastructure object may itself be stored as a payload owner here. */
XRAY_API XrtManagedStatus xrt_place_borrow(XrtPlace, const XrtManagedHeader **);
/* The callback publishes only non-owning payload bytes after all incoming owner
 * fields are installed and before any replaced owner is retired. It must return
 * normally, allocate nothing, perform no Root/Place operation and never write
 * owner fields, descriptors or busy domains. Its borrowed context and prepared
 * byte snapshots stay live throughout this synchronous call. NULL omits it;
 * a supplied descriptor requires a callback. Rejection never invokes it. */
typedef struct XrtPlacePublication {
    void (*publish)(void *);
    void *context;
} XrtPlacePublication;
XRAY_API XrtManagedStatus xrt_place_copy_replace(XrtPlace destination, XrtPlace source,
    const XrtPlacePublication *);
XRAY_API XrtManagedStatus xrt_place_move_replace(XrtPlace destination, XrtPlace source,
    const XrtPlacePublication *);

typedef enum XrtPlaceTransfer {
    XRT_PLACE_TRANSFER_COPY = 1, XRT_PLACE_TRANSFER_MOVE
} XrtPlaceTransfer;
/* One selected edge, not a sequence of pairwise swaps. Destinations are unique;
 * a source may have any number of COPY rows and at most one MOVE row. Sources
 * and destinations may overlap. Temporary places are distinct empty Root slots,
 * disjoint from both lists. Zero is not an admitted transfer operation.
 * After complete preflight, every COPY retains into its temporary. A failed
 * retain rolls back all such references without changing sources/destinations.
 * Only then do MOVE rows consume their sources into temporaries. All incoming
 * owners and non-owning bytes publish before any replaced owner retires.
 * Lists/descriptors remain unchanged throughout this allocation-free call. */
typedef struct XrtPlaceEdge {
    XrtPlace destination, source, temporary;
    XrtPlaceTransfer transfer;
} XrtPlaceEdge;
XRAY_API XrtManagedStatus xrt_place_edge_transfer(const XrtPlaceEdge *, size_t count,
    const XrtPlacePublication *);
#ifdef __cplusplus
}
#endif
#endif // XRAY_NATIVE_PLACE_H
