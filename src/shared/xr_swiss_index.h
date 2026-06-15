/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_swiss_index.h - Shared Swiss-style control-byte probing primitives.
 */

#ifndef XR_SWISS_INDEX_H
#define XR_SWISS_INDEX_H

#include <stdint.h>
#include <string.h>

#define XR_SWISS_GROUP 8
#define XR_SWISS_CTRL_EMPTY 0xFFu
#define XR_SWISS_CTRL_DELETED 0x80u
#define XR_SWISS_SWAR_LOW 0x0101010101010101ull
#define XR_SWISS_SWAR_HIGH 0x8080808080808080ull

static inline uint8_t xr_swiss_h2(uint64_t hash) {
    return (uint8_t) (hash & 0x7Fu);
}

static inline uint64_t xr_swiss_group_load(const uint8_t *p) {
    uint64_t g;
    memcpy(&g, p, sizeof(g));
    return g;
}

static inline uint64_t xr_swiss_group_match(uint64_t group, uint8_t h2) {
    uint64_t x = group ^ (XR_SWISS_SWAR_LOW * (uint64_t) h2);
    return (x - XR_SWISS_SWAR_LOW) & ~x & XR_SWISS_SWAR_HIGH;
}

static inline uint64_t xr_swiss_group_match_empty(uint64_t group) {
    return group & (group << 1) & XR_SWISS_SWAR_HIGH;
}

static inline uint64_t xr_swiss_group_match_free(uint64_t group) {
    return group & XR_SWISS_SWAR_HIGH;
}

static inline int xr_swiss_swar_first(uint64_t bits) {
    int n = 0;
    while (!(bits & 0xFFu)) {
        bits >>= 8;
        n++;
    }
    return n;
}

static inline void xr_swiss_ctrl_set(uint8_t *ctrl, uint32_t slots, uint32_t slot, uint8_t value) {
    ctrl[slot] = value;
    if (slot < XR_SWISS_GROUP)
        ctrl[slots + slot] = value;
}

static inline uint32_t xr_swiss_find_free(const uint8_t *ctrl, uint32_t slots, uint64_t hash) {
    uint32_t mask = slots - 1u;
    uint32_t pos = (uint32_t) ((hash >> 7u) & mask);
    uint32_t stride = 0;

    for (;;) {
        uint64_t group = xr_swiss_group_load(ctrl + pos);
        uint64_t free_mask = xr_swiss_group_match_free(group);
        if (free_mask) {
            int off = xr_swiss_swar_first(free_mask);
            return (pos + (uint32_t) off) & mask;
        }
        stride += XR_SWISS_GROUP;
        pos = (pos + stride) & mask;
    }
}

#endif  // XR_SWISS_INDEX_H
