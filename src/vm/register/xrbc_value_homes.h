/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_value_homes.h - Raw indexed homes for stable activation payloads
 *
 * KEY CONCEPT:
 *   Code owns only ordinals, offsets and verified event positions. A prepared
 *   execution owns plans and frames; movable register banks only borrow them.
 */
#ifndef XRBC_VALUE_HOMES_H
#define XRBC_VALUE_HOMES_H
#include "runtime/native/xnative_place.h"
#include "lir/xlir_value_homes.h"
#include "xrbc.h"

typedef struct XrRbcHomeLocation {
    const XrLirValueHomes *homes;
    XrtManagedFrame *frame;
    uint32_t value;
    bool temporary;
} XrRbcHomeLocation;

/* Only the receiving compiler creates and destroys these immutable code assets.
 * No runtime plan, Ops, Layout, module pointer or actual owner is retained here. */
/* The physical function index is zero-based. The immutable module snapshot must
 * carry the compiler's live code lease or explicit validated owner-free raw
 * descriptions for the entire borrow. Scalar-only functions may have no
 * descriptor. Cold homes stay outside the hot
 * function table so adding physical metadata cannot change interpreter stride. */
XR_FUNC const XrLirValueHomes *xr_rbc_module_value_homes(const XrRbcModule *, uint32_t function);
/* The same lease or the raw caller's explicit immutable storage protects this
 * metadata borrow. It grants no owning slot or escaping environment. */
XR_FUNC const XrtValueImage *xr_rbc_module_value_image(const XrRbcModule *);
/* A scalar-only or entirely borrowed activation creates no frame and performs
 * no payload initialization. Formal parameters borrow their caller's normalized
 * stable places; they never acquire references merely to enter a function. */
XR_FUNC XrtManagedStatus xr_rbc_value_homes_enter(const XrLirValueHomes *,
    XrtManagedRoot *, XrtManagedFrame **empty);
/* These resolve an activation's own home, never a borrowed formal. The address
 * is stable until its real frame retires. Neither operation grants mutation or
 * guest borrowing authority; typed NULL policy remains explicit at each use. */
XR_FUNC XrtManagedStatus xr_rbc_value_home_address(XrRbcHomeLocation, void **empty);
XR_FUNC XrtManagedStatus xr_rbc_value_home_place(XrRbcHomeLocation, size_t leaf,
    XrtPlaceNull, XrtPlace *);
/* Project a typed, admitted operand to its sole physical owner slot. Zero
 * means the origin must be resolved dynamically. A one-based slot still
 * reads the current Frame, never a cached backing pointer. */
static inline uint32_t rbc_local_borrow_slot(const XrLirValueHomes *homes, uint32_t value) {
    if (!homes || !homes->values || !value || value > homes->value_count ||
        (homes->bindings && homes->bindings[value].type)) return 0;
    const XrLirValueHome *home = &homes->values[value];
    if (home->parameter || home->borrow_source || home->owner_count != 1 ||
        home->size != sizeof(XrtManagedHeader *)) return 0;
    size_t slot = home->literal_cache_borrow ? home->literal_cache_owner : home->first_owner;
    size_t offset = home->literal_cache_borrow ? home->literal_cache_offset : home->offset;
    if (offset == SIZE_MAX || slot >= UINT32_MAX || slot >= homes->frame.slot_count ||
        !homes->frame.owned_offsets || homes->frame.owned_offsets[slot] != offset) return 0;
    return (uint32_t)slot + 1;
}
/* The binding's type and write authority are checked separately. Only a
 * local receiving Frame provides a one-based slot for the in-place edit. */
static inline uint32_t rbc_local_map_binding_slot(const XrLirValueHomes *homes, uint32_t value) {
    if (!homes || !homes->bindings || !value || value > homes->value_count) return 0;
    const XrLirHomeBinding *binding = &homes->bindings[value];
    size_t slot = binding->first_owner;
    if (binding->kind != XR_LIR_HOME_BINDING_LOCAL || slot >= UINT32_MAX ||
        slot >= homes->frame.slot_count || !homes->frame.owned_offsets ||
        homes->frame.owned_offsets[slot] != binding->offset) return 0;
    return (uint32_t)slot + 1;
}
#endif // XRBC_VALUE_HOMES_H
