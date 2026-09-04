/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xdiscovery_announcement.c - Native datagram projection of cluster.xr policy
 */

#include "xdiscovery_announcement.h"
#include "xcluster_wire.h"

#include <stdbool.h>
#include <string.h>

static bool printable_ascii(const uint8_t *text, size_t length) {
    for (size_t i = 0; i < length; i++) {
        if (text[i] < 0x20 || text[i] > 0x7E)
            return false;
    }
    return true;
}

int xr_discovery_announcement_encode(uint8_t *buf, size_t capacity, const char *name, uint16_t port,
                                     uint64_t cluster_hash) {
    if (!buf || !name || port == 0)
        return -1;
    size_t name_len_wide = strlen(name);
    if (name_len_wide == 0 || name_len_wide > XR_NODE_NAME_MAX ||
        !printable_ascii((const uint8_t *) name, name_len_wide))
        return -1;
    uint8_t name_len = (uint8_t) name_len_wide;
    size_t total = 4u + 1u + 1u + name_len + 2u + 8u;
    if (total > capacity)
        return -1;

    uint8_t *p = buf;
    p[0] = (uint8_t) (XR_DISCOVERY_MAGIC >> 24);
    p[1] = (uint8_t) (XR_DISCOVERY_MAGIC >> 16);
    p[2] = (uint8_t) (XR_DISCOVERY_MAGIC >> 8);
    p[3] = (uint8_t) XR_DISCOVERY_MAGIC;
    p += 4;
    *p++ = XR_DISCOVERY_VERSION;
    *p++ = name_len;
    memcpy(p, name, name_len);
    p += name_len;
    *p++ = (uint8_t) (port >> 8);
    *p++ = (uint8_t) port;
    for (int i = 7; i >= 0; i--)
        *p++ = (uint8_t) (cluster_hash >> (i * 8));
    return (int) total;
}

int xr_discovery_announcement_decode(const uint8_t *buf, size_t length, char *name,
                                     size_t name_capacity, uint16_t *port, uint64_t *cluster_hash) {
    if (!buf || !name || !port || !cluster_hash || length < 16u)
        return -1;
    uint32_t magic =
        ((uint32_t) buf[0] << 24) | ((uint32_t) buf[1] << 16) | ((uint32_t) buf[2] << 8) | buf[3];
    if (magic != XR_DISCOVERY_MAGIC || buf[4] != XR_DISCOVERY_VERSION)
        return -1;
    uint8_t name_len = buf[5];
    size_t exact_length = 4u + 1u + 1u + name_len + 2u + 8u;
    if (name_len == 0 || name_len > XR_NODE_NAME_MAX || exact_length != length ||
        (size_t) name_len + 1u > name_capacity)
        return -1;
    if (!printable_ascii(buf + 6, name_len))
        return -1;

    memcpy(name, buf + 6, name_len);
    name[name_len] = '\0';
    const uint8_t *p = buf + 6 + name_len;
    *port = (uint16_t) (((uint16_t) p[0] << 8) | p[1]);
    if (*port == 0)
        return -1;
    p += 2;
    uint64_t hash = 0;
    for (int i = 0; i < 8; i++)
        hash = (hash << 8) | p[i];
    *cluster_hash = hash;
    return 0;
}
