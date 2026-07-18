/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_simd_lower.h - Lower the portable simd module surface to typed Xi ops
 */

#ifndef XI_SIMD_LOWER_H
#define XI_SIMD_LOWER_H

#include "xaot_bundle.h"
#include "../base/xdefs.h"
#include <stdbool.h>

/* Run only after xaot_prepare + XAOT_VERIFY_AOT_READY.  The prepared sidecar
 * owns the resolved aggregate representations that imported simd values need;
 * rewriting earlier would discard that cross-module ABI proof. */
XR_FUNC bool xi_simd_lower_bundle(XaotBundle *bundle, uint32_t *lowered_count);

#endif /* XI_SIMD_LOWER_H */
