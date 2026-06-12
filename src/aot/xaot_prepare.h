/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xaot_prepare.h - AOT prepare pass
 */

#ifndef XAOT_PREPARE_H
#define XAOT_PREPARE_H

#include "xaot_bundle.h"

XR_FUNC bool xaot_prepare_bundle(XaotBundle *bundle, XaotPrepareStats *out_stats);

/* In-bounds proof for an XI_INDEX_GET / XI_INDEX_SET access.  Returns the
 * XAOT_BOUNDS_EV_* evidence bits when proven, 0 otherwise; when unproven
 * and out_reason is non-NULL it receives the XAOT_BOUNDS_UNPROVEN_* reason.
 * The bounds-plan pass records both outcomes; the verifier re-derives them. */
XR_FUNC uint32_t xaot_prepare_array_access_bounds_evidence(const XaotBundle *bundle,
                                                           const XiFunc *func,
                                                           const XiValue *access,
                                                           uint8_t *out_reason);

/* Uniqueness proof for an array data cache (XAOT_ALIAS_UNIQUE_DATA).
 * Returns the XAOT_ALIAS_EV_* evidence bits when the cached data pointer is
 * provably the only element-storage pointer in the function, 0 otherwise.
 * The alias-plan pass records positive results; the verifier re-derives
 * them.  Requires bounds plans to be populated first. */
XR_FUNC uint32_t xaot_prepare_array_cache_alias_evidence(const XaotBundle *bundle,
                                                         const XiFunc *func,
                                                         const XaotArrayCachePlan *cache_plan);

#endif  // XAOT_PREPARE_H
