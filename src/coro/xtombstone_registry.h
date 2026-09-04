/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtombstone_registry.h - Expiring synchronized name tombstones
 */

#ifndef XTOMBSTONE_REGISTRY_H
#define XTOMBSTONE_REGISTRY_H

#include "../base/xdefs.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct XrTombstoneRegistry XrTombstoneRegistry;

XR_FUNC XrTombstoneRegistry *xr_tombstone_registry_new(uint32_t initial_capacity,
                                                       int64_t retention_ms);
XR_FUNC void xr_tombstone_registry_destroy(XrTombstoneRegistry *registry);
XR_FUNC bool xr_tombstone_registry_add(XrTombstoneRegistry *registry, const char *name,
                                       int64_t now_ms);
XR_FUNC bool xr_tombstone_registry_contains(XrTombstoneRegistry *registry, const char *name,
                                            int64_t now_ms);
XR_FUNC int64_t xr_tombstone_registry_count(XrTombstoneRegistry *registry, int64_t now_ms);

#endif  // XTOMBSTONE_REGISTRY_H
