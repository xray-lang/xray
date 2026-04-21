/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * http_compress.c - HTTP compression/decompression (thin wrapper)
 *
 * KEY CONCEPT:
 *   Delegates to the unified compress module (compress_zlib.c).
 *   Retains the http-specific API surface (XrCompressType dispatch,
 *   Content-Encoding detection, pooled gzip) for backward compat.
 */

#include "http_compress.h"
#include "../compress/compress.h"

/* ========== Compression Type Detection ========== */

XrCompressType xr_detect_compress_type(const char *content_encoding) {
    /* XrContentEncoding values (0,1,2) map directly to XrCompressType */
    return (XrCompressType)xr_detect_content_encoding(content_encoding);
}

bool xr_compress_available(void) {
    return true;  /* system zlib always linked (-lz) */
}

/* ========== Decompression ========== */

int xr_gzip_decompress(const void *in, size_t in_len,
                        void **out, size_t *out_len) {
    return xr_zlib_gzip_decompress(in, in_len, out, out_len);
}

int xr_deflate_decompress(const void *in, size_t in_len,
                           void **out, size_t *out_len) {
    return xr_zlib_deflate_decompress(in, in_len, out, out_len);
}

int xr_decompress(XrCompressType type,
                  const void *in, size_t in_len,
                  void **out, size_t *out_len) {
    switch (type) {
        case XR_COMPRESS_GZIP:
            return xr_zlib_gzip_decompress(in, in_len, out, out_len);
        case XR_COMPRESS_DEFLATE:
            return xr_zlib_deflate_decompress(in, in_len, out, out_len);
        case XR_COMPRESS_NONE:
        default:
            return -1;
    }
}

/* ========== Compression ========== */

int xr_gzip_compress(const void *in, size_t in_len,
                     void **out, size_t *out_len, int level) {
    return xr_zlib_gzip_compress(in, in_len, out, out_len, level);
}

int xr_deflate_compress(const void *in, size_t in_len,
                        void **out, size_t *out_len, int level) {
    return xr_zlib_deflate_compress(in, in_len, out, out_len, level);
}

int xr_compress(XrCompressType type, int level,
                const void *in, size_t in_len,
                void **out, size_t *out_len) {
    switch (type) {
        case XR_COMPRESS_GZIP:
            return xr_zlib_gzip_compress(in, in_len, out, out_len, level);
        case XR_COMPRESS_DEFLATE:
            return xr_zlib_deflate_compress(in, in_len, out, out_len, level);
        case XR_COMPRESS_NONE:
        default:
            return -1;
    }
}

/* ========== Compressor Object Pool ========== */

/*
 * The pool previously maintained a fixed array of z_stream contexts
 * guarded by a pthread_mutex. Now that the unified compress module
 * handles zlib lifecycle, the pool is a simple pass-through to the
 * non-pooled path. High-frequency HTTP compression pipelines that
 * need amortised init overhead can use the stream API directly.
 */
void xr_compress_pool_init(void) { /* no-op */ }
void xr_compress_pool_shutdown(void) { /* no-op */ }

int xr_gzip_compress_pooled(const void *in, size_t in_len,
                            void **out, size_t *out_len, int level) {
    return xr_zlib_gzip_compress(in, in_len, out, out_len, level);
}
