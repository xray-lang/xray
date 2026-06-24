/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtransfer_mode.h - Explicit cross-coroutine transfer modes
 */

#ifndef XTRANSFER_MODE_H
#define XTRANSFER_MODE_H

#include <stdint.h>

typedef enum XrTransferMode {
    XR_TRANSFER_SHARE = 0, /* already safe to share: scalar, immutable, shared const, handle */
    XR_TRANSFER_COPY = 1,  /* explicit copy(expr): clone once at the boundary */
    XR_TRANSFER_MOVE = 2,  /* explicit move expr: transfer ownership, no clone */
} XrTransferMode;

#define XR_TRANSFER_MODE_BITS 2u
#define XR_TRANSFER_MODE_MASK 0x3u
#define XR_TRANSFER_MODES_PER_U32 16u

static inline uint32_t xr_transfer_pack_mode(uint32_t packed, uint32_t slot, uint8_t mode) {
    uint32_t shift = (slot % XR_TRANSFER_MODES_PER_U32) * XR_TRANSFER_MODE_BITS;
    packed &= ~(XR_TRANSFER_MODE_MASK << shift);
    packed |= ((uint32_t) mode & XR_TRANSFER_MODE_MASK) << shift;
    return packed;
}

static inline uint8_t xr_transfer_unpack_mode(uint32_t packed, uint32_t slot) {
    uint32_t shift = (slot % XR_TRANSFER_MODES_PER_U32) * XR_TRANSFER_MODE_BITS;
    return (uint8_t) ((packed >> shift) & XR_TRANSFER_MODE_MASK);
}

#endif  // XTRANSFER_MODE_H
