/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_value_image.h - Owned raw descriptions from the common receiving plans
 */
#ifndef XLIR_VALUE_IMAGE_H
#define XLIR_VALUE_IMAGE_H
#include "xlir_value_layout.h"
#include "xray_native_value_image.h"

typedef struct XrLirValueImage XrLirValueImage;
/* The input set stays live and immutable throughout this synchronous call.
 * Type IDs are its enumeration ordinal plus one, shared with homes and C type
 * declarations. One enumeration and one temporary dependency index copy the
 * sealed physical facts; no backend computes layout or child identities again.
 * The raw schema owns only tables and names. Its private owner keeps the real
 * billing/provider lease and exact extent, never a Plan, module or Layout.
 * Executable code leases remain a separate binding input.
 * Output starts NULL. Failure preserves it and refunds all metadata. An empty
 * set succeeds without allocation; view(NULL) is the canonical empty image. */
XR_FUNC XrLirValueLayoutStatus xr_lir_value_image_create(const XrLirValueLayouts *,
    XrtBillingOwner *, XrLirValueImage **empty);
XR_FUNC const XrtValueImage *xr_lir_value_image_view(const XrLirValueImage *);
XR_FUNC void xr_lir_value_image_destroy(XrLirValueImage *);
#endif // XLIR_VALUE_IMAGE_H
