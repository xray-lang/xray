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
 * carry the compiler's live code lease for the entire borrow. Raw modules and
 * scalar-only functions have no descriptor. Cold homes stay outside the hot
 * function table so adding physical metadata cannot change interpreter stride. */
XR_FUNC const XrLirValueHomes *xr_rbc_module_value_homes(const XrRbcModule *, uint32_t function);
/* The same real code lease protects this raw metadata borrow. Scalar-only and
 * foreign borrowed modules have no compiler-owned image. */
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
#endif // XRBC_VALUE_HOMES_H
