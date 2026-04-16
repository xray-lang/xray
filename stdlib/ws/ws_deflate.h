/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * ws_deflate.h - WebSocket permessage-deflate (RFC 7692)
 *
 * KEY CONCEPT:
 *   Per-message compression using raw deflate with Z_SYNC_FLUSH.
 *   Currently supports no_context_takeover mode only (stateless,
 *   each message compressed independently).
 */

#ifndef XR_WS_DEFLATE_H
#define XR_WS_DEFLATE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/*
 * Compress payload for permessage-deflate.
 * Uses raw deflate + Z_SYNC_FLUSH, strips trailing 0x00 0x00 0xff 0xff.
 * Caller must free(*out) when done.
 * Returns 0 on success, -1 on error.
 */
int xr_ws_deflate_compress(const uint8_t *in, size_t in_len,
                           uint8_t **out, size_t *out_len);

/*
 * Decompress permessage-deflate payload.
 * Appends 0x00 0x00 0xff 0xff trailer before inflating.
 * Caller must free(*out) when done.
 * Returns 0 on success, -1 on error.
 */
int xr_ws_deflate_decompress(const uint8_t *in, size_t in_len,
                             uint8_t **out, size_t *out_len);

#endif // XR_WS_DEFLATE_H
