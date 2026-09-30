/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_interface_members.h - Exact inherited requirement consistency
 */
#ifndef XXIR_INTERFACE_MEMBERS_H
#define XXIR_INTERFACE_MEMBERS_H
#include "xxir_interface.h"

/* Requires structurally verified descriptors and acyclic interface declarations.
 * Checks every declaration, including unused ones. Original declarations retain
 * all distinct obligations; temporary application deduplication loses no identity.
 * Failure preserves the caller budget and publishes no partial result. */
XR_FUNC XrXirStatus xr_xir_interfaces_verify_members_verified(
    const XrXirInterfaceTable *table, const XrXirTypes *types, XrXirBudget *budget);
#endif // XXIR_INTERFACE_MEMBERS_H
