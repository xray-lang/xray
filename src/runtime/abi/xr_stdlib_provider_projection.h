/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_stdlib_provider_projection.h - Explicit typed provider ABI projection
 */

#ifndef XR_STDLIB_PROVIDER_PROJECTION_H
#define XR_STDLIB_PROVIDER_PROJECTION_H

#include "xr_stdlib_provider_contract.h"
#include "xr_runtime_contract.h"
#include "xr_target_machine_facts.h"

typedef enum XrStdlibProviderProjectionStatus {
    XR_STDLIB_PROVIDER_PROJECTION_OK = 0,
    XR_STDLIB_PROVIDER_PROJECTION_UNAVAILABLE,
    XR_STDLIB_PROVIDER_PROJECTION_INVALID,
} XrStdlibProviderProjectionStatus;

/* The descriptor is borrowed from the generated registry. Only explicit target
 * facts determine physical slots; failure leaves the output unchanged. */
XR_FUNC XrStdlibProviderProjectionStatus xr_stdlib_provider_project(
    const XrStdlibProviderDescriptor *descriptor, const XrTargetMachineFacts *machine,
    XrTargetProviderOperationContract *out);

#endif  // XR_STDLIB_PROVIDER_PROJECTION_H
