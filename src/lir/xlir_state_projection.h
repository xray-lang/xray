/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_state_projection.h - One physical module-slot projection for C and RBC
 *
 * KEY CONCEPT:
 *   Logical slot ordinals survive while their storage uses the same receiving
 *   value plans and one-based image ids as function homes and typed C values.
 */
#ifndef XLIR_STATE_PROJECTION_H
#define XLIR_STATE_PROJECTION_H
#include "xlir_state.h"
#include "xlir_value_layout.h"
#include "xray_native_state.h"

typedef struct XrLirStateProjection XrLirStateProjection;
/* The module is already admitted. Its legacy transport offsets are not the
 * physical layout. The set supplies all field layouts and ids; this producer
 * only aligns complete State slots. The raw image must be serialized from that
 * exact set and stay alive through the projection's final borrow. CodeImage
 * owns both raw owners, never the set, runtime plans, Layout or State. C emits
 * both static descriptors from these same facts. All copied slot/module rows
 * and dependencies use one billed allocation and a real provider lease.
 * No module or plan pointer survives. An absent State succeeds without an
 * allocation and leaves *empty NULL. Fn/Cell/View slots remain unsupported. */
XR_FUNC XrLirValueLayoutStatus xr_lir_state_projection_create(const XrLirModule *,
    const XrLirValueLayouts *, const XrtValueImage *, XrtBillingOwner *, XrLirStateProjection **empty);
XR_FUNC const XrtModuleLayoutSpec *xr_lir_state_projection_view(const XrLirStateProjection *);
XR_FUNC void xr_lir_state_projection_destroy(XrLirStateProjection *);
#endif // XLIR_STATE_PROJECTION_H
