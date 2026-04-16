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
 *   Uses system zlib for raw deflate with Z_SYNC_FLUSH.
 *   no_context_takeover: each message is independently compressed.
 *
 * WHY THIS DESIGN:
 *   - System zlib is universally available and battle-tested
 *   - Z_SYNC_FLUSH produces the required 0x00 0x00 0xff 0xff trailer
 *   - Stripping/appending the trailer follows RFC 7692 Section 7.2.1
 */

#include "ws_deflate.h"
#include <zlib.h>
#include <stdlib.h>
#include <string.h>

// RFC 7692: trailing bytes appended by Z_SYNC_FLUSH that must be stripped
static const uint8_t DEFLATE_TRAILER[4] = {0x00, 0x00, 0xff, 0xff};

int xr_ws_deflate_compress(const uint8_t *in, size_t in_len,
                           uint8_t **out, size_t *out_len)
{
    if (!in || !out || !out_len) return -1;

    z_stream zs;
    memset(&zs, 0, sizeof(zs));

    // Raw deflate: -15 = raw, no zlib/gzip header
    if (deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
                     -15, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        return -1;
    }

    // Worst case: deflate can expand data slightly
    size_t bound = deflateBound(&zs, (uLong)in_len);
    uint8_t *buf = (uint8_t *)malloc(bound);
    if (!buf) {
        deflateEnd(&zs);
        return -1;
    }

    zs.next_in = (Bytef *)in;
    zs.avail_in = (uInt)in_len;
    zs.next_out = buf;
    zs.avail_out = (uInt)bound;

    // Z_SYNC_FLUSH produces 0x00 0x00 0xff 0xff at the end
    int rc = deflate(&zs, Z_SYNC_FLUSH);
    deflateEnd(&zs);

    if (rc != Z_OK || zs.avail_in != 0) {
        free(buf);
        return -1;
    }

    size_t compressed_len = (size_t)(zs.next_out - buf);

    // Strip trailing 0x00 0x00 0xff 0xff (RFC 7692 Section 7.2.1)
    if (compressed_len >= 4 &&
        memcmp(buf + compressed_len - 4, DEFLATE_TRAILER, 4) == 0) {
        compressed_len -= 4;
    }

    *out = buf;
    *out_len = compressed_len;
    return 0;
}

int xr_ws_deflate_decompress(const uint8_t *in, size_t in_len,
                             uint8_t **out, size_t *out_len)
{
    if (!in || !out || !out_len) return -1;

    // Append the stripped trailer before inflating
    size_t total_in = in_len + 4;
    uint8_t *input = (uint8_t *)malloc(total_in);
    if (!input) return -1;
    memcpy(input, in, in_len);
    memcpy(input + in_len, DEFLATE_TRAILER, 4);

    z_stream zs;
    memset(&zs, 0, sizeof(zs));

    // Raw inflate: -15 = raw deflate stream
    if (inflateInit2(&zs, -15) != Z_OK) {
        free(input);
        return -1;
    }

    // Start with 4x input size, grow as needed
    size_t buf_cap = in_len * 4;
    if (buf_cap < 256) buf_cap = 256;
    uint8_t *buf = (uint8_t *)malloc(buf_cap);
    if (!buf) {
        inflateEnd(&zs);
        free(input);
        return -1;
    }

    zs.next_in = input;
    zs.avail_in = (uInt)total_in;
    zs.next_out = buf;
    zs.avail_out = (uInt)buf_cap;

    while (1) {
        int rc = inflate(&zs, Z_SYNC_FLUSH);
        if (rc == Z_STREAM_END || (rc == Z_OK && zs.avail_in == 0)) {
            break;
        }
        if (rc == Z_OK && zs.avail_out == 0) {
            // Need more output space
            size_t used = buf_cap;
            buf_cap *= 2;
            uint8_t *new_buf = (uint8_t *)realloc(buf, buf_cap);
            if (!new_buf) {
                free(buf);
                inflateEnd(&zs);
                free(input);
                return -1;
            }
            buf = new_buf;
            zs.next_out = buf + used;
            zs.avail_out = (uInt)(buf_cap - used);
            continue;
        }
        // Error
        free(buf);
        inflateEnd(&zs);
        free(input);
        return -1;
    }

    size_t decompressed_len = (size_t)(zs.next_out - buf);
    inflateEnd(&zs);
    free(input);

    *out = buf;
    *out_len = decompressed_len;
    return 0;
}
