/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * ws_deflate.c - WebSocket permessage-deflate (RFC 7692)
 *
 * KEY CONCEPT:
 *   Thin wrapper over stdlib/compress/compress_zlib.c stream API.
 *   Delegates all zlib interaction to the unified compress module,
 *   keeping WS-specific logic (trailer strip/append, zip-bomb cap)
 *   in xr_deflate_sync_flush / xr_inflate_bounded.
 *
 *   no_context_takeover: each message is independently compressed.
 */

#include "ws_deflate.h"
#include "../compress/compress.h"

int xr_ws_deflate_compress(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    return xr_deflate_sync_flush(in, in_len, out, out_len, 6 /* Z_DEFAULT_COMPRESSION */);
}

int xr_ws_deflate_decompress(const uint8_t *in, size_t in_len, size_t max_out, uint8_t **out,
                             size_t *out_len) {
    return xr_inflate_bounded(in, in_len, max_out, out, out_len);
}
