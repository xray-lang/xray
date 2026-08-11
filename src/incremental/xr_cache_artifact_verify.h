/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_cache_artifact_verify.h - Plan-owned verification adapters for cache hits
 */

#ifndef XR_CACHE_ARTIFACT_VERIFY_H
#define XR_CACHE_ARTIFACT_VERIFY_H

#include "xr_cache_store.h"

/* This adapter accepts only XSM artifacts and delegates every schema and
 * semantic decision to the owning decoder. XTP support must arrive through
 * its own frozen decoder instead of being reconstructed in the cache layer. */
XR_FUNC bool xr_cache_verify_xsm_artifact(XrCacheArtifactKind kind, XrCacheKey key,
                                          const uint8_t *bytes, size_t size, void *context);

#endif  // XR_CACHE_ARTIFACT_VERIFY_H
