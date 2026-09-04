/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xdiscovery_announcement.h - Allocation-free LAN announcement projection
 */

#ifndef XR_IO_DISCOVERY_ANNOUNCEMENT_H
#define XR_IO_DISCOVERY_ANNOUNCEMENT_H

#include "../base/xdefs.h"

#include <stddef.h>
#include <stdint.h>

#define XR_DISCOVERY_MCAST_GROUP "239.42.42.42"
#define XR_DISCOVERY_MCAST_PORT 47200
#define XR_DISCOVERY_INTERVAL_MS 3000
#define XR_DISCOVERY_MAGIC 0x58524459u
#define XR_DISCOVERY_VERSION 2
#define XR_DISCOVERY_ANNOUNCEMENT_MAX_SIZE 128

XR_FUNC int xr_discovery_announcement_encode(uint8_t *buf, size_t capacity, const char *name,
                                             uint16_t port, uint64_t cluster_hash);
XR_FUNC int xr_discovery_announcement_decode(const uint8_t *buf, size_t length, char *name,
                                             size_t name_capacity, uint16_t *port,
                                             uint64_t *cluster_hash);

#endif /* XR_IO_DISCOVERY_ANNOUNCEMENT_H */
