/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * http_compress.h - HTTP compression/decompression support
 *
 * KEY CONCEPT:
 *   gzip/deflate compression, compressor object pooling,
 *   and automatic Content-Encoding detection.
 */

#ifndef XR_STDLIB_HTTP_COMPRESS_H
#define XR_STDLIB_HTTP_COMPRESS_H

#include <stddef.h>
#include <stdbool.h>
#include "../compress/compress.h"

// Compression level
typedef enum {
    XR_COMPRESS_LEVEL_NONE = 0,
    XR_COMPRESS_LEVEL_FAST = 1,
    XR_COMPRESS_LEVEL_DEFAULT = 6,
    XR_COMPRESS_LEVEL_BEST = 9
} XrCompressLevel;

/*
 * Detect compression type from Content-Encoding header.
 * Returns XrContentEncoding (defined in compress.h).
 */
XrContentEncoding xr_detect_compress_type(const char *content_encoding);

/*
 * Decompress data
 *
 * type: Compression type
 * in: Compressed data
 * in_len: Compressed data length
 * out: Output buffer (caller must free)
 * out_len: Output length
 *
 * Returns: 0 on success, -1 on failure
 */
int xr_decompress(XrContentEncoding type,
                  const void *in, size_t in_len,
                  void **out, size_t *out_len);

/*
 * gzip decompress
 */
int xr_gzip_decompress(const void *in, size_t in_len,
                        void **out, size_t *out_len);

/*
 * deflate decompress
 */
int xr_deflate_decompress(const void *in, size_t in_len,
                           void **out, size_t *out_len);

/*
 * Check if zlib is available
 */
bool xr_compress_available(void);

/* ========== Compression API ========== */

/*
 * gzip compress
 * level: Compression level (1-9)
 */
int xr_gzip_compress(const void *in, size_t in_len,
                     void **out, size_t *out_len,
                     int level);

/*
 * deflate compress
 */
int xr_deflate_compress(const void *in, size_t in_len,
                        void **out, size_t *out_len,
                        int level);

/*
 * Generic compression interface
 */
int xr_compress(XrContentEncoding type, int level,
                const void *in, size_t in_len,
                void **out, size_t *out_len);

/* ========== Compressor Object Pool ========== */

/*
 * Initialize compressor pool
 */
void xr_compress_pool_init(void);

/*
 * Shutdown compressor pool
 */
void xr_compress_pool_shutdown(void);

/*
 * Compress using pooled compressor (reduces memory allocation)
 */
int xr_gzip_compress_pooled(const void *in, size_t in_len,
                            void **out, size_t *out_len,
                            int level);

#endif
