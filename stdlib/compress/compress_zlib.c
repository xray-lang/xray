/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * compress_zlib.c - System zlib wrapper: stateful stream API
 *
 * KEY CONCEPT:
 *   Thin layer over system zlib (-lz) that:
 *     - routes all allocations through xr_malloc/xr_free,
 *     - exposes explicit flush modes (Z_SYNC_FLUSH / Z_FINISH)
 *       so WebSocket permessage-deflate (RFC 7692) and HTTP
 *       Content-Encoding can share a single code path, and
 *     - enforces an output cap on inflate to defend against
 *       zip-bomb attacks.
 *
 * WHY A SEPARATE FILE:
 *   compress.c implements RFC 1951 deflate from scratch in pure C
 *   (no external dependency). This file complements it with zlib-
 *   backed streaming for callers (ws_deflate, http_compress) that
 *   need Z_SYNC_FLUSH or gzip framing — capabilities the from-
 *   scratch implementation does not (yet) expose. Both files are
 *   compiled into the same `compress` module.
 */

#include "compress.h"
#include "../../src/base/xmalloc.h"
#include "../../src/base/xchecks.h"
#include "../../src/base/xstrcompat.h"

#include <zlib.h>
#include <string.h>

/* ========== Custom zlib allocator routing ========== */

static voidpf zlib_alloc(voidpf opaque, uInt items, uInt size) {
    (void) opaque;
    return xr_calloc((size_t) items, (size_t) size);
}

static void zlib_free(voidpf opaque, voidpf address) {
    (void) opaque;
    xr_free(address);
}

/* ========== XrZlibStream — Stateful Compress/Decompress ========== */

struct XrZlibStream {
    z_stream zs;
    bool is_deflate; /* true = deflate (compress), false = inflate */
    bool finished;   /* Z_STREAM_END received or emitted */
};

XR_FUNC XrZlibStream *xr_zlib_stream_new_deflate(int level, int window_bits) {
    if (level < 0)
        level = Z_DEFAULT_COMPRESSION;
    if (level > 9)
        level = 9;

    XrZlibStream *s = (XrZlibStream *) xr_calloc(1, sizeof(XrZlibStream));
    if (!s)
        return NULL;

    s->zs.zalloc = zlib_alloc;
    s->zs.zfree = zlib_free;
    s->zs.opaque = NULL;
    s->is_deflate = true;

    int rc = deflateInit2(&s->zs, level, Z_DEFLATED, window_bits, 8, Z_DEFAULT_STRATEGY);
    if (rc != Z_OK) {
        xr_free(s);
        return NULL;
    }
    return s;
}

XR_FUNC XrZlibStream *xr_zlib_stream_new_inflate(int window_bits) {
    XrZlibStream *s = (XrZlibStream *) xr_calloc(1, sizeof(XrZlibStream));
    if (!s)
        return NULL;

    s->zs.zalloc = zlib_alloc;
    s->zs.zfree = zlib_free;
    s->zs.opaque = NULL;
    s->is_deflate = false;

    int rc = inflateInit2(&s->zs, window_bits);
    if (rc != Z_OK) {
        xr_free(s);
        return NULL;
    }
    return s;
}

XR_FUNC int xr_zlib_stream_process(XrZlibStream *s, const uint8_t *in, size_t in_len, uint8_t *out,
                                   size_t out_cap, size_t *out_written, int flush) {
    if (!s || !out || !out_written)
        return -1;

    s->zs.next_in = (Bytef *) (in ? in : (const uint8_t *) "");
    s->zs.avail_in = (uInt) (in ? in_len : 0);
    s->zs.next_out = (Bytef *) out;
    s->zs.avail_out = (uInt) out_cap;

    int rc;
    if (s->is_deflate) {
        rc = deflate(&s->zs, flush);
    } else {
        rc = inflate(&s->zs, flush);
    }

    *out_written = out_cap - (size_t) s->zs.avail_out;

    if (rc == Z_STREAM_END) {
        s->finished = true;
        return 1; /* stream complete */
    }
    if (rc == Z_OK || rc == Z_BUF_ERROR) {
        return 0; /* OK, may need more input or output space */
    }
    return -1; /* error */
}

XR_FUNC bool xr_zlib_stream_finished(const XrZlibStream *s) {
    return s ? s->finished : true;
}

XR_FUNC void xr_zlib_stream_free(XrZlibStream *s) {
    if (!s)
        return;
    if (s->is_deflate) {
        deflateEnd(&s->zs);
    } else {
        inflateEnd(&s->zs);
    }
    xr_free(s);
}

/* ========== Convenience: one-shot raw deflate + Z_SYNC_FLUSH ========== */

/*
 * Used by WebSocket permessage-deflate (RFC 7692). Compresses the
 * entire input in one pass using Z_SYNC_FLUSH, then strips the
 * mandatory 0x00 0x00 0xff 0xff trailer per Section 7.2.1.
 *
 * Returns 0 on success. Caller must xr_free(*out).
 */
XR_FUNC int xr_deflate_sync_flush(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len,
                                  int level) {
    if (!in || !out || !out_len)
        return -1;

    XrZlibStream *s = xr_zlib_stream_new_deflate(level, -15);
    if (!s)
        return -1;

    size_t bound = (size_t) deflateBound(&s->zs, (uLong) in_len);
    uint8_t *buf = (uint8_t *) xr_malloc(bound);
    if (!buf) {
        xr_zlib_stream_free(s);
        return -1;
    }

    size_t written = 0;
    int rc = xr_zlib_stream_process(s, in, in_len, buf, bound, &written, Z_SYNC_FLUSH);
    xr_zlib_stream_free(s);

    if (rc < 0) {
        xr_free(buf);
        return -1;
    }

    /* Strip trailing 0x00 0x00 0xff 0xff (RFC 7692 §7.2.1) */
    static const uint8_t TRAILER[4] = {0x00, 0x00, 0xff, 0xff};
    if (written >= 4 && memcmp(buf + written - 4, TRAILER, 4) == 0) {
        written -= 4;
    }

    *out = buf;
    *out_len = written;
    return 0;
}

/* ========== Convenience: one-shot raw inflate with output cap ========== */

/*
 * Used by WebSocket permessage-deflate. Prepends the RFC 7692 trailer
 * to the input, then inflates with a hard output cap (zip-bomb guard).
 *
 * Returns 0 on success. Caller must xr_free(*out).
 */
XR_FUNC int xr_inflate_bounded(const uint8_t *in, size_t in_len, size_t max_out, uint8_t **out,
                               size_t *out_len) {
    if (!in || !out || !out_len)
        return -1;

    /* Append the stripped trailer before inflating */
    static const uint8_t TRAILER[4] = {0x00, 0x00, 0xff, 0xff};
    size_t total_in = in_len + 4;
    uint8_t *input = (uint8_t *) xr_malloc(total_in);
    if (!input)
        return -1;
    memcpy(input, in, in_len);
    memcpy(input + in_len, TRAILER, 4);

    XrZlibStream *s = xr_zlib_stream_new_inflate(-15);
    if (!s) {
        xr_free(input);
        return -1;
    }

    /* Start at 4x, grow up to max_out */
    size_t buf_cap = in_len * 4;
    if (buf_cap < 256)
        buf_cap = 256;
    if (max_out > 0 && buf_cap > max_out)
        buf_cap = max_out;

    uint8_t *buf = (uint8_t *) xr_malloc(buf_cap);
    if (!buf) {
        xr_zlib_stream_free(s);
        xr_free(input);
        return -1;
    }

    s->zs.next_in = input;
    s->zs.avail_in = (uInt) total_in;

    size_t total_written = 0;

    while (1) {
        s->zs.next_out = buf + total_written;
        s->zs.avail_out = (uInt) (buf_cap - total_written);

        int rc = inflate(&s->zs, Z_SYNC_FLUSH);

        total_written = buf_cap - (size_t) s->zs.avail_out;

        if (rc == Z_STREAM_END || (rc == Z_OK && s->zs.avail_in == 0)) {
            break;
        }
        if (rc == Z_OK && s->zs.avail_out == 0) {
            /* Zip-bomb guard */
            if (max_out > 0 && total_written >= max_out) {
                xr_free(buf);
                xr_zlib_stream_free(s);
                xr_free(input);
                return -1;
            }
            size_t new_cap = buf_cap * 2;
            if (max_out > 0 && new_cap > max_out)
                new_cap = max_out;
            uint8_t *tmp = (uint8_t *) xr_realloc(buf, new_cap);
            if (!tmp) {
                xr_free(buf);
                xr_zlib_stream_free(s);
                xr_free(input);
                return -1;
            }
            buf = tmp;
            buf_cap = new_cap;
            continue;
        }
        /* Error */
        xr_free(buf);
        xr_zlib_stream_free(s);
        xr_free(input);
        return -1;
    }

    xr_zlib_stream_free(s);
    xr_free(input);

    /* Final bound check (defence in depth) */
    if (max_out > 0 && total_written > max_out) {
        xr_free(buf);
        return -1;
    }

    *out = buf;
    *out_len = total_written;
    return 0;
}

/* ========== HTTP helpers: gzip/deflate with auto-alloc ========== */

XR_FUNC int xr_zlib_gzip_compress(const void *in, size_t in_len, void **out, size_t *out_len,
                                  int level) {
    if (!in || in_len == 0 || !out || !out_len)
        return -1;
    if (level < 1)
        level = 1;
    if (level > 9)
        level = 9;

    /* 16 + MAX_WBITS = gzip format */
    XrZlibStream *s = xr_zlib_stream_new_deflate(level, 16 + MAX_WBITS);
    if (!s)
        return -1;

    size_t bound = (size_t) deflateBound(&s->zs, (uLong) in_len) + 18;
    uint8_t *buf = (uint8_t *) xr_malloc(bound);
    if (!buf) {
        xr_zlib_stream_free(s);
        return -1;
    }

    size_t written = 0;
    int rc =
        xr_zlib_stream_process(s, (const uint8_t *) in, in_len, buf, bound, &written, Z_FINISH);
    xr_zlib_stream_free(s);

    if (rc != 1) { /* must be Z_STREAM_END */
        xr_free(buf);
        return -1;
    }

    *out = buf;
    *out_len = written;
    return 0;
}

XR_FUNC int xr_zlib_gzip_decompress(const void *in, size_t in_len, void **out, size_t *out_len) {
    if (!in || in_len == 0 || !out || !out_len)
        return -1;

    /* 16 + MAX_WBITS = gzip format */
    XrZlibStream *s = xr_zlib_stream_new_inflate(16 + MAX_WBITS);
    if (!s)
        return -1;

    size_t buf_cap = in_len * 4;
    if (buf_cap < 1024)
        buf_cap = 1024;
    uint8_t *buf = (uint8_t *) xr_malloc(buf_cap);
    if (!buf) {
        xr_zlib_stream_free(s);
        return -1;
    }

    s->zs.next_in = (Bytef *) in;
    s->zs.avail_in = (uInt) in_len;

    size_t total = 0;

    while (1) {
        s->zs.next_out = buf + total;
        s->zs.avail_out = (uInt) (buf_cap - total);

        int rc = inflate(&s->zs, Z_NO_FLUSH);
        total = buf_cap - (size_t) s->zs.avail_out;

        if (rc == Z_STREAM_END)
            break;
        if (rc == Z_OK && s->zs.avail_out == 0) {
            size_t new_cap = buf_cap * 2;
            uint8_t *tmp = (uint8_t *) xr_realloc(buf, new_cap);
            if (!tmp) {
                xr_free(buf);
                xr_zlib_stream_free(s);
                return -1;
            }
            buf = tmp;
            buf_cap = new_cap;
            continue;
        }
        if (rc != Z_OK) {
            xr_free(buf);
            xr_zlib_stream_free(s);
            return -1;
        }
    }

    xr_zlib_stream_free(s);

    *out = buf;
    *out_len = total;
    return 0;
}

XR_FUNC int xr_zlib_deflate_compress(const void *in, size_t in_len, void **out, size_t *out_len,
                                     int level) {
    if (!in || in_len == 0 || !out || !out_len)
        return -1;
    if (level < 1)
        level = 1;
    if (level > 9)
        level = 9;

    /* -MAX_WBITS = raw deflate */
    XrZlibStream *s = xr_zlib_stream_new_deflate(level, -MAX_WBITS);
    if (!s)
        return -1;

    size_t bound = (size_t) deflateBound(&s->zs, (uLong) in_len);
    uint8_t *buf = (uint8_t *) xr_malloc(bound);
    if (!buf) {
        xr_zlib_stream_free(s);
        return -1;
    }

    size_t written = 0;
    int rc =
        xr_zlib_stream_process(s, (const uint8_t *) in, in_len, buf, bound, &written, Z_FINISH);
    xr_zlib_stream_free(s);

    if (rc != 1) {
        xr_free(buf);
        return -1;
    }

    *out = buf;
    *out_len = written;
    return 0;
}

XR_FUNC int xr_zlib_deflate_decompress(const void *in, size_t in_len, void **out, size_t *out_len) {
    if (!in || in_len == 0 || !out || !out_len)
        return -1;

    /* Try raw deflate first, then zlib-wrapped */
    XrZlibStream *s = xr_zlib_stream_new_inflate(-MAX_WBITS);
    if (!s)
        return -1;

    size_t buf_cap = in_len * 4;
    if (buf_cap < 1024)
        buf_cap = 1024;
    uint8_t *buf = (uint8_t *) xr_malloc(buf_cap);
    if (!buf) {
        xr_zlib_stream_free(s);
        return -1;
    }

    s->zs.next_in = (Bytef *) in;
    s->zs.avail_in = (uInt) in_len;

    size_t total = 0;

    while (1) {
        s->zs.next_out = buf + total;
        s->zs.avail_out = (uInt) (buf_cap - total);

        int rc = inflate(&s->zs, Z_NO_FLUSH);
        total = buf_cap - (size_t) s->zs.avail_out;

        if (rc == Z_STREAM_END)
            break;
        if (rc == Z_OK && s->zs.avail_out == 0) {
            size_t new_cap = buf_cap * 2;
            uint8_t *tmp = (uint8_t *) xr_realloc(buf, new_cap);
            if (!tmp) {
                xr_free(buf);
                xr_zlib_stream_free(s);
                return -1;
            }
            buf = tmp;
            buf_cap = new_cap;
            continue;
        }
        if (rc != Z_OK) {
            xr_free(buf);
            xr_zlib_stream_free(s);
            return -1;
        }
    }

    xr_zlib_stream_free(s);

    *out = buf;
    *out_len = total;
    return 0;
}

/* ========== Content-Encoding detection (moved from http_compress) ========== */

XR_FUNC XrContentEncoding xr_detect_content_encoding(const char *encoding) {
    if (!encoding)
        return XR_CONTENT_ENC_NONE;
    if (xr_strcasecmp(encoding, "gzip") == 0 || xr_strcasecmp(encoding, "x-gzip") == 0) {
        return XR_CONTENT_ENC_GZIP;
    }
    if (xr_strcasecmp(encoding, "deflate") == 0) {
        return XR_CONTENT_ENC_DEFLATE;
    }
    return XR_CONTENT_ENC_NONE;
}
