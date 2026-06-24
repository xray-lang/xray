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

static inline int64_t xr_swiss_slots_for_i64(int64_t want) {
    int64_t slots = XR_SWISS_GROUP;
    if (want < 0)
        want = 0;
    while (slots - slots / 8 < want)
        slots <<= 1;
    return slots;
}

static inline int64_t xr_swiss_capacity_budget_i64(int64_t slots) {
    return slots - slots / 8;
}

static inline void xr_swiss_ctrl_set_i64(uint8_t *ctrl, int64_t slots, int64_t slot,
                                         uint8_t value) {
    ctrl[slot] = value;
    if (slot < XR_SWISS_GROUP)
        ctrl[slots + slot] = value;
}

static inline void xr_swiss_ctrl_set(uint8_t *ctrl, uint32_t slots, uint32_t slot, uint8_t value) {
    xr_swiss_ctrl_set_i64(ctrl, (int64_t) slots, (int64_t) slot, value);
}

/* Find the slot holding `h2`-tagged keys; `eq_slot(ctx, slot)` confirms false
 * positives from the control byte. Returns the slot index or -1. */
typedef int (*XrSwissEqI64Fn)(void *ctx, int64_t slot);

static inline int64_t xr_swiss_find_match_i64(const uint8_t *ctrl, int64_t slots, uint64_t hash,
                                              XrSwissEqI64Fn eq, void *ctx) {
    uint64_t mask = (uint64_t) slots - 1u;
    uint8_t h2 = xr_swiss_h2(hash);
    uint64_t pos = (hash >> 7u) & mask;
    uint64_t stride = 0;

    for (;;) {
        uint64_t group = xr_swiss_group_load(ctrl + pos);
        uint64_t matches = xr_swiss_group_match(group, h2);
        while (matches) {
            int off = xr_swiss_swar_first(matches);
            int64_t slot = (int64_t) ((pos + (uint64_t) off) & mask);
            if (eq(ctx, slot))
                return slot;
            matches &= ~(0xFFull << ((unsigned) off * 8u));
        }
        if (xr_swiss_group_match_empty(group))
            return -1;
        stride += XR_SWISS_GROUP;
        pos = (pos + stride) & mask;
    }
}

static inline int64_t xr_swiss_find_free_i64(const uint8_t *ctrl, int64_t slots, uint64_t hash) {
    uint64_t mask = (uint64_t) slots - 1u;
    uint64_t pos = (hash >> 7u) & mask;
    uint64_t stride = 0;

    for (;;) {
        uint64_t group = xr_swiss_group_load(ctrl + pos);
        uint64_t free_mask = xr_swiss_group_match_free(group);
        if (free_mask) {
            int off = xr_swiss_swar_first(free_mask);
            return (int64_t) ((pos + (uint64_t) off) & mask);
        }
        stride += XR_SWISS_GROUP;
        pos = (pos + stride) & mask;
    }
}

/* First EMPTY (never DELETED) slot along the probe sequence. Callers that keep
 * external insertion-order arrays use this to avoid reusing tombstones until a
 * compact rehash drops stale order entries. */
static inline int64_t xr_swiss_find_empty_i64(const uint8_t *ctrl, int64_t slots, uint64_t hash) {
    uint64_t mask = (uint64_t) slots - 1u;
    uint64_t pos = (hash >> 7u) & mask;
    uint64_t stride = 0;

    for (;;) {
        uint64_t group = xr_swiss_group_load(ctrl + pos);
        uint64_t empty_mask = xr_swiss_group_match_empty(group);
        if (empty_mask) {
            int off = xr_swiss_swar_first(empty_mask);
            return (int64_t) ((pos + (uint64_t) off) & mask);
        }
        stride += XR_SWISS_GROUP;
        pos = (pos + stride) & mask;
    }
}

static inline uint32_t xr_swiss_find_free(const uint8_t *ctrl, uint32_t slots, uint64_t hash) {
    return (uint32_t) xr_swiss_find_free_i64(ctrl, (int64_t) slots, hash);
}

/* Insert a dense entry index into a fresh EMPTY-or-DELETED control slot for a
 * key/value already proven absent. Shared by the VM and AOT map/set insert and
 * rehash paths. */
static inline void xr_swiss_indices_put(uint8_t *ctrl, int32_t *indices, uint32_t indices_size,
                                        uint32_t hash, int32_t eidx) {
    uint32_t slot = xr_swiss_find_free(ctrl, indices_size, hash);
    indices[slot] = eidx;
    xr_swiss_ctrl_set(ctrl, indices_size, slot, xr_swiss_h2(hash));
}

#endif  // XR_SWISS_INDEX_H
