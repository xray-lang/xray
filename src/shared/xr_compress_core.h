/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_compress_core.h - Pure checksum helpers shared by VM stdlib and AOT
 */

#ifndef XR_COMPRESS_CORE_H
#define XR_COMPRESS_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../base/xdefs.h"

#ifndef XR_COMPRESS_LEVELS_DEFINED
#define XR_COMPRESS_LEVELS_DEFINED
#define XR_COMPRESS_NO_COMPRESSION 0
#define XR_COMPRESS_BEST_SPEED 1
#define XR_COMPRESS_BEST_COMPRESSION 9
#define XR_COMPRESS_DEFAULT_COMPRESSION 6
#endif

typedef struct {
    const uint8_t *data;
    size_t len;
} XrCompressCoreInputView;

static inline int xr_compress_core_level_or_default(bool has_level, int64_t raw_level) {
    if (!has_level)
        return XR_COMPRESS_DEFAULT_COMPRESSION;
    if (raw_level < XR_COMPRESS_NO_COMPRESSION)
        return XR_COMPRESS_NO_COMPRESSION;
    if (raw_level > XR_COMPRESS_BEST_COMPRESSION)
        return XR_COMPRESS_BEST_COMPRESSION;
    return (int) raw_level;
}

static inline bool xr_compress_core_input_view(const char *data, int64_t len,
                                               XrCompressCoreInputView *out) {
    if (!out)
        return false;
    if (len < 0)
        return false;
    if (!data && len != 0)
        return false;

    out->data = (const uint8_t *) (data ? data : "");
    out->len = (size_t) len;
    return true;
}

#ifndef XR_COMPRESS_ERROR_DEFINED
#define XR_COMPRESS_ERROR_DEFINED
typedef enum {
    XR_COMPRESS_OK = 0,
    XR_COMPRESS_ERR_MEMORY,
    XR_COMPRESS_ERR_DATA,
    XR_COMPRESS_ERR_BUFFER,
    XR_COMPRESS_ERR_STREAM,
    XR_COMPRESS_ERR_HEADER,
    XR_COMPRESS_ERR_CHECKSUM
} XrCompressError;
#endif

#define XR_COMPRESS_CORE_GZIP_WRAPPER_BYTES 18u
#define XR_COMPRESS_CORE_ZLIB_WRAPPER_BYTES 6u
#define XR_COMPRESS_CORE_GUNZIP_FALLBACK_MULTIPLIER 4u
#define XR_COMPRESS_CORE_GUNZIP_PAD 256u
#define XR_COMPRESS_CORE_GUNZIP_MAX_TRIES 4
#define XR_COMPRESS_CORE_INFLATE_FALLBACK_MULTIPLIER 8u
#define XR_COMPRESS_CORE_INFLATE_PAD 1024u
#define XR_COMPRESS_CORE_INFLATE_MAX_TRIES 8

static const uint32_t XR_COMPRESS_CORE_CRC32_TABLE[256] = {
    0x00000000, 0x77073096, 0xEE0E612C, 0x990951BA, 0x076DC419, 0x706AF48F, 0xE963A535, 0x9E6495A3,
    0x0EDB8832, 0x79DCB8A4, 0xE0D5E91E, 0x97D2D988, 0x09B64C2B, 0x7EB17CBD, 0xE7B82D07, 0x90BF1D91,
    0x1DB71064, 0x6AB020F2, 0xF3B97148, 0x84BE41DE, 0x1ADAD47D, 0x6DDDE4EB, 0xF4D4B551, 0x83D385C7,
    0x136C9856, 0x646BA8C0, 0xFD62F97A, 0x8A65C9EC, 0x14015C4F, 0x63066CD9, 0xFA0F3D63, 0x8D080DF5,
    0x3B6E20C8, 0x4C69105E, 0xD56041E4, 0xA2677172, 0x3C03E4D1, 0x4B04D447, 0xD20D85FD, 0xA50AB56B,
    0x35B5A8FA, 0x42B2986C, 0xDBBBC9D6, 0xACBCF940, 0x32D86CE3, 0x45DF5C75, 0xDCD60DCF, 0xABD13D59,
    0x26D930AC, 0x51DE003A, 0xC8D75180, 0xBFD06116, 0x21B4F4B5, 0x56B3C423, 0xCFBA9599, 0xB8BDA50F,
    0x2802B89E, 0x5F058808, 0xC60CD9B2, 0xB10BE924, 0x2F6F7C87, 0x58684C11, 0xC1611DAB, 0xB6662D3D,
    0x76DC4190, 0x01DB7106, 0x98D220BC, 0xEFD5102A, 0x71B18589, 0x06B6B51F, 0x9FBFE4A5, 0xE8B8D433,
    0x7807C9A2, 0x0F00F934, 0x9609A88E, 0xE10E9818, 0x7F6A0DBB, 0x086D3D2D, 0x91646C97, 0xE6635C01,
    0x6B6B51F4, 0x1C6C6162, 0x856530D8, 0xF262004E, 0x6C0695ED, 0x1B01A57B, 0x8208F4C1, 0xF50FC457,
    0x65B0D9C6, 0x12B7E950, 0x8BBEB8EA, 0xFCB9887C, 0x62DD1DDF, 0x15DA2D49, 0x8CD37CF3, 0xFBD44C65,
    0x4DB26158, 0x3AB551CE, 0xA3BC0074, 0xD4BB30E2, 0x4ADFA541, 0x3DD895D7, 0xA4D1C46D, 0xD3D6F4FB,
    0x4369E96A, 0x346ED9FC, 0xAD678846, 0xDA60B8D0, 0x44042D73, 0x33031DE5, 0xAA0A4C5F, 0xDD0D7CC9,
    0x5005713C, 0x270241AA, 0xBE0B1010, 0xC90C2086, 0x5768B525, 0x206F85B3, 0xB966D409, 0xCE61E49F,
    0x5EDEF90E, 0x29D9C998, 0xB0D09822, 0xC7D7A8B4, 0x59B33D17, 0x2EB40D81, 0xB7BD5C3B, 0xC0BA6CAD,
    0xEDB88320, 0x9ABFB3B6, 0x03B6E20C, 0x74B1D29A, 0xEAD54739, 0x9DD277AF, 0x04DB2615, 0x73DC1683,
    0xE3630B12, 0x94643B84, 0x0D6D6A3E, 0x7A6A5AA8, 0xE40ECF0B, 0x9309FF9D, 0x0A00AE27, 0x7D079EB1,
    0xF00F9344, 0x8708A3D2, 0x1E01F268, 0x6906C2FE, 0xF762575D, 0x806567CB, 0x196C3671, 0x6E6B06E7,
    0xFED41B76, 0x89D32BE0, 0x10DA7A5A, 0x67DD4ACC, 0xF9B9DF6F, 0x8EBEEFF9, 0x17B7BE43, 0x60B08ED5,
    0xD6D6A3E8, 0xA1D1937E, 0x38D8C2C4, 0x4FDFF252, 0xD1BB67F1, 0xA6BC5767, 0x3FB506DD, 0x48B2364B,
    0xD80D2BDA, 0xAF0A1B4C, 0x36034AF6, 0x41047A60, 0xDF60EFC3, 0xA867DF55, 0x316E8EEF, 0x4669BE79,
    0xCB61B38C, 0xBC66831A, 0x256FD2A0, 0x5268E236, 0xCC0C7795, 0xBB0B4703, 0x220216B9, 0x5505262F,
    0xC5BA3BBE, 0xB2BD0B28, 0x2BB45A92, 0x5CB36A04, 0xC2D7FFA7, 0xB5D0CF31, 0x2CD99E8B, 0x5BDEAE1D,
    0x9B64C2B0, 0xEC63F226, 0x756AA39C, 0x026D930A, 0x9C0906A9, 0xEB0E363F, 0x72076785, 0x05005713,
    0x95BF4A82, 0xE2B87A14, 0x7BB12BAE, 0x0CB61B38, 0x92D28E9B, 0xE5D5BE0D, 0x7CDCEFB7, 0x0BDBDF21,
    0x86D3D2D4, 0xF1D4E242, 0x68DDB3F8, 0x1FDA836E, 0x81BE16CD, 0xF6B9265B, 0x6FB077E1, 0x18B74777,
    0x88085AE6, 0xFF0F6A70, 0x66063BCA, 0x11010B5C, 0x8F659EFF, 0xF862AE69, 0x616BFFD3, 0x166CCF45,
    0xA00AE278, 0xD70DD2EE, 0x4E048354, 0x3903B3C2, 0xA7672661, 0xD06016F7, 0x4969474D, 0x3E6E77DB,
    0xAED16A4A, 0xD9D65ADC, 0x40DF0B66, 0x37D83BF0, 0xA9BCAE53, 0xDEBB9EC5, 0x47B2CF7F, 0x30B5FFE9,
    0xBDBDF21C, 0xCABAC28A, 0x53B39330, 0x24B4A3A6, 0xBAD03605, 0xCDD70693, 0x54DE5729, 0x23D967BF,
    0xB3667A2E, 0xC4614AB8, 0x5D681B02, 0x2A6F2B94, 0xB40BBE37, 0xC30C8EA1, 0x5A05DF1B, 0x2D02EF8D,
};

static inline uint32_t xr_compress_core_crc32_update(uint32_t crc, const uint8_t *data,
                                                     size_t len) {
    crc = ~crc;
    for (size_t i = 0; i < len; i++)
        crc = XR_COMPRESS_CORE_CRC32_TABLE[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

static inline uint32_t xr_compress_core_crc32(const uint8_t *data, size_t len) {
    return xr_compress_core_crc32_update(0, data, len);
}

#define XR_COMPRESS_CORE_ADLER_MOD 65521u
#define XR_COMPRESS_CORE_ADLER_NMAX 5552u

static inline uint32_t xr_compress_core_adler32_update(uint32_t adler, const uint8_t *data,
                                                       size_t len) {
    uint32_t a = adler & 0xFFFFu;
    uint32_t b = (adler >> 16) & 0xFFFFu;

    while (len > 0) {
        size_t chunk = len >= XR_COMPRESS_CORE_ADLER_NMAX ? XR_COMPRESS_CORE_ADLER_NMAX : len;
        len -= chunk;
        for (size_t i = 0; i < chunk; i++) {
            a += data[i];
            b += a;
        }
        a %= XR_COMPRESS_CORE_ADLER_MOD;
        b %= XR_COMPRESS_CORE_ADLER_MOD;
        data += chunk;
    }

    return (b << 16) | a;
}

static inline uint32_t xr_compress_core_adler32(const uint8_t *data, size_t len) {
    return xr_compress_core_adler32_update(1, data, len);
}

XR_FUNC XrCompressError xr_deflate(const uint8_t *input, size_t in_len, uint8_t *output,
                                   size_t out_cap, size_t *out_len, int level);
XR_FUNC XrCompressError xr_inflate(const uint8_t *input, size_t in_len, uint8_t *output,
                                   size_t out_cap, size_t *out_len);
XR_FUNC size_t xr_deflate_bound(size_t in_len);
XR_FUNC XrCompressError xr_gzip(const uint8_t *input, size_t in_len, uint8_t *output,
                                size_t out_cap, size_t *out_len, int level);
XR_FUNC XrCompressError xr_gunzip(const uint8_t *input, size_t in_len, uint8_t *output,
                                  size_t out_cap, size_t *out_len);
XR_FUNC bool xr_is_gzip(const uint8_t *data, size_t len);
XR_FUNC uint32_t xr_gzip_original_size(const uint8_t *data, size_t len);
XR_FUNC XrCompressError xr_zlib_compress(const uint8_t *input, size_t in_len, uint8_t *output,
                                         size_t out_cap, size_t *out_len, int level);
XR_FUNC XrCompressError xr_zlib_decompress(const uint8_t *input, size_t in_len, uint8_t *output,
                                           size_t out_cap, size_t *out_len);
XR_FUNC bool xr_is_zlib(const uint8_t *data, size_t len);

typedef void *(*XrCompressCoreAllocFn)(void *ctx, size_t size);
typedef void (*XrCompressCoreFreeFn)(void *ctx, void *ptr);
typedef XrCompressError (*XrCompressCoreCompressFn)(const uint8_t *input, size_t in_len,
                                                    uint8_t *output, size_t out_cap,
                                                    size_t *out_len, int level);
typedef XrCompressError (*XrCompressCoreDecompressFn)(const uint8_t *input, size_t in_len,
                                                      uint8_t *output, size_t out_cap,
                                                      size_t *out_len);

static inline bool xr_compress_core_checked_add(size_t a, size_t b, size_t *out) {
    if (!out || a > SIZE_MAX - b)
        return false;
    *out = a + b;
    return true;
}

static inline bool xr_compress_core_checked_mul(size_t a, size_t b, size_t *out) {
    if (!out || (b != 0 && a > SIZE_MAX / b))
        return false;
    *out = a * b;
    return true;
}

static inline bool xr_compress_core_compress_bound(size_t in_len, size_t wrapper_bytes,
                                                   size_t *out_cap) {
    return xr_compress_core_checked_add(xr_deflate_bound(in_len), wrapper_bytes, out_cap);
}

static inline bool xr_compress_core_gunzip_initial_cap(size_t in_len, uint32_t original_size,
                                                       size_t *out_cap) {
    size_t base = (size_t) original_size;
    if (base == 0 &&
        !xr_compress_core_checked_mul(in_len, XR_COMPRESS_CORE_GUNZIP_FALLBACK_MULTIPLIER, &base))
        return false;
    return xr_compress_core_checked_add(base, XR_COMPRESS_CORE_GUNZIP_PAD, out_cap);
}

static inline bool xr_compress_core_inflate_initial_cap(size_t in_len, size_t *out_cap) {
    size_t base = 0;
    if (!xr_compress_core_checked_mul(in_len, XR_COMPRESS_CORE_INFLATE_FALLBACK_MULTIPLIER, &base))
        return false;
    return xr_compress_core_checked_add(base, XR_COMPRESS_CORE_INFLATE_PAD, out_cap);
}

static inline bool xr_compress_core_next_cap(size_t cap, size_t *next_cap) {
    return xr_compress_core_checked_mul(cap, 2u, next_cap);
}

static inline uint8_t *xr_compress_core_compress_alloc(const uint8_t *input, size_t in_len,
                                                       int level, size_t wrapper_bytes,
                                                       XrCompressCoreCompressFn fn,
                                                       XrCompressCoreAllocFn alloc_fn,
                                                       XrCompressCoreFreeFn free_fn,
                                                       void *alloc_ctx, size_t *out_len) {
    if (!fn || !alloc_fn || !free_fn || !out_len)
        return NULL;
    *out_len = 0;
    size_t bound = 0;
    if (!xr_compress_core_compress_bound(in_len, wrapper_bytes, &bound))
        return NULL;
    uint8_t *output = (uint8_t *) alloc_fn(alloc_ctx, bound);
    if (!output)
        return NULL;
    XrCompressError err = fn(input, in_len, output, bound, out_len, level);
    if (err != XR_COMPRESS_OK) {
        free_fn(alloc_ctx, output);
        return NULL;
    }
    return output;
}

static inline uint8_t *xr_compress_core_decompress_alloc(const uint8_t *input, size_t in_len,
                                                         size_t initial_cap, int max_tries,
                                                         XrCompressCoreDecompressFn fn,
                                                         XrCompressCoreAllocFn alloc_fn,
                                                         XrCompressCoreFreeFn free_fn,
                                                         void *alloc_ctx, size_t *out_len) {
    if (!fn || !alloc_fn || !free_fn || !out_len || initial_cap == 0 || max_tries <= 0)
        return NULL;
    *out_len = 0;
    size_t cap = initial_cap;
    for (int tries = 0; tries < max_tries; tries++) {
        uint8_t *output = (uint8_t *) alloc_fn(alloc_ctx, cap);
        if (!output)
            return NULL;

        XrCompressError err = fn(input, in_len, output, cap, out_len);
        if (err == XR_COMPRESS_OK)
            return output;
        free_fn(alloc_ctx, output);
        if (err != XR_COMPRESS_ERR_BUFFER)
            return NULL;
        if (!xr_compress_core_next_cap(cap, &cap))
            return NULL;
    }
    return NULL;
}

static inline uint8_t *xr_compress_core_gzip_alloc(const uint8_t *input, size_t in_len,
                                                   size_t *out_len, int level,
                                                   XrCompressCoreAllocFn alloc_fn,
                                                   XrCompressCoreFreeFn free_fn, void *alloc_ctx) {
    return xr_compress_core_compress_alloc(input, in_len, level,
                                           XR_COMPRESS_CORE_GZIP_WRAPPER_BYTES, xr_gzip, alloc_fn,
                                           free_fn, alloc_ctx, out_len);
}

static inline uint8_t *xr_compress_core_deflate_alloc(const uint8_t *input, size_t in_len,
                                                      size_t *out_len, int level,
                                                      XrCompressCoreAllocFn alloc_fn,
                                                      XrCompressCoreFreeFn free_fn,
                                                      void *alloc_ctx) {
    return xr_compress_core_compress_alloc(input, in_len, level, 0, xr_deflate, alloc_fn, free_fn,
                                           alloc_ctx, out_len);
}

static inline uint8_t *xr_compress_core_zlib_compress_alloc(const uint8_t *input, size_t in_len,
                                                            size_t *out_len, int level,
                                                            XrCompressCoreAllocFn alloc_fn,
                                                            XrCompressCoreFreeFn free_fn,
                                                            void *alloc_ctx) {
    return xr_compress_core_compress_alloc(input, in_len, level,
                                           XR_COMPRESS_CORE_ZLIB_WRAPPER_BYTES, xr_zlib_compress,
                                           alloc_fn, free_fn, alloc_ctx, out_len);
}

static inline uint8_t *xr_compress_core_gunzip_alloc(const uint8_t *input, size_t in_len,
                                                     uint32_t original_size, size_t *out_len,
                                                     XrCompressCoreAllocFn alloc_fn,
                                                     XrCompressCoreFreeFn free_fn,
                                                     void *alloc_ctx) {
    size_t cap = 0;
    if (!xr_compress_core_gunzip_initial_cap(in_len, original_size, &cap))
        return NULL;
    return xr_compress_core_decompress_alloc(input, in_len, cap, XR_COMPRESS_CORE_GUNZIP_MAX_TRIES,
                                             xr_gunzip, alloc_fn, free_fn, alloc_ctx, out_len);
}

static inline uint8_t *xr_compress_core_inflate_alloc(const uint8_t *input, size_t in_len,
                                                      size_t *out_len,
                                                      XrCompressCoreAllocFn alloc_fn,
                                                      XrCompressCoreFreeFn free_fn,
                                                      void *alloc_ctx) {
    size_t cap = 0;
    if (!xr_compress_core_inflate_initial_cap(in_len, &cap))
        return NULL;
    return xr_compress_core_decompress_alloc(input, in_len, cap, XR_COMPRESS_CORE_INFLATE_MAX_TRIES,
                                             xr_inflate, alloc_fn, free_fn, alloc_ctx, out_len);
}

static inline uint8_t *xr_compress_core_zlib_decompress_alloc(const uint8_t *input, size_t in_len,
                                                              size_t *out_len,
                                                              XrCompressCoreAllocFn alloc_fn,
                                                              XrCompressCoreFreeFn free_fn,
                                                              void *alloc_ctx) {
    size_t cap = 0;
    if (!xr_compress_core_inflate_initial_cap(in_len, &cap))
        return NULL;
    return xr_compress_core_decompress_alloc(input, in_len, cap, XR_COMPRESS_CORE_INFLATE_MAX_TRIES,
                                             xr_zlib_decompress, alloc_fn, free_fn, alloc_ctx,
                                             out_len);
}

#endif  // XR_COMPRESS_CORE_H
