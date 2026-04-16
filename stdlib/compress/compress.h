/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * compress.h - Compression standard library
 *
 * KEY CONCEPT:
 *   Provides gzip (RFC 1952), deflate (RFC 1951), and zlib (RFC 1950)
 *   compression/decompression with CRC32 and Adler32 checksums.
 *
 * USAGE:
 *   import compress
 *   let compressed = compress.gzip(data)
 *   let original = compress.gunzip(compressed)
 */

#ifndef XR_STDLIB_COMPRESS_H
#define XR_STDLIB_COMPRESS_H

#include "../../src/module/xmodule.h"
#include "../../src/vm/xvm.h"
#include "../../src/runtime/object/xstring.h"

/* ========== Compression Levels ========== */

#define XR_COMPRESS_NO_COMPRESSION      0
#define XR_COMPRESS_BEST_SPEED          1
#define XR_COMPRESS_BEST_COMPRESSION    9
#define XR_COMPRESS_DEFAULT_COMPRESSION 6

/* ========== Error Codes ========== */

typedef enum {
    XR_COMPRESS_OK = 0,
    XR_COMPRESS_ERR_MEMORY,
    XR_COMPRESS_ERR_DATA,
    XR_COMPRESS_ERR_BUFFER,
    XR_COMPRESS_ERR_STREAM,
    XR_COMPRESS_ERR_HEADER,
    XR_COMPRESS_ERR_CHECKSUM
} XrCompressError;

/* ========== Deflate Compression/Decompression ========== */

// Raw deflate compression (no header)
XrCompressError xr_deflate(const uint8_t *input, size_t in_len,
                           uint8_t *output, size_t out_cap, size_t *out_len,
                           int level);

// Raw deflate decompression
XrCompressError xr_inflate(const uint8_t *input, size_t in_len,
                           uint8_t *output, size_t out_cap, size_t *out_len);

// Estimate maximum compressed size
size_t xr_deflate_bound(size_t in_len);

/* ========== Gzip Compression/Decompression ========== */

// Gzip compression
XrCompressError xr_gzip(const uint8_t *input, size_t in_len,
                        uint8_t *output, size_t out_cap, size_t *out_len,
                        int level);

// Gzip decompression
XrCompressError xr_gunzip(const uint8_t *input, size_t in_len,
                          uint8_t *output, size_t out_cap, size_t *out_len);

// Check if data is valid gzip format
bool xr_is_gzip(const uint8_t *data, size_t len);

// Get original size from gzip trailer (only lower 32 bits for large files)
uint32_t xr_gzip_original_size(const uint8_t *data, size_t len);

/* ========== Zlib Format Compression/Decompression ========== */

// Zlib compression (with header and checksum)
XrCompressError xr_zlib_compress(const uint8_t *input, size_t in_len,
                                  uint8_t *output, size_t out_cap, size_t *out_len,
                                  int level);

// Zlib decompression
XrCompressError xr_zlib_decompress(const uint8_t *input, size_t in_len,
                                    uint8_t *output, size_t out_cap, size_t *out_len);

// Check if data is valid zlib format
bool xr_is_zlib(const uint8_t *data, size_t len);

/* ========== CRC32 Checksum ========== */

// Compute CRC32 checksum (used by gzip)
uint32_t xr_crc32(const uint8_t *data, size_t len);

// Incremental CRC32 computation
uint32_t xr_crc32_update(uint32_t crc, const uint8_t *data, size_t len);

/* ========== Adler32 Checksum ========== */

// Compute Adler32 checksum (used by zlib)
uint32_t xr_adler32(const uint8_t *data, size_t len);

// Incremental Adler32 computation
uint32_t xr_adler32_update(uint32_t adler, const uint8_t *data, size_t len);

/* ========== Heap-Allocated Versions ========== */

// Gzip compression with automatic memory allocation
// Returns compressed data (caller must free), NULL on failure
uint8_t* xr_gzip_alloc(const uint8_t *input, size_t in_len, 
                        size_t *out_len, int level);

// Gunzip decompression with automatic memory allocation
// Returns decompressed data (caller must free), NULL on failure
uint8_t* xr_gunzip_alloc(const uint8_t *input, size_t in_len, size_t *out_len);

/* ========== Error Message ========== */

const char* xr_compress_error_str(XrCompressError err);

/* ========== Module Loading ========== */

XrModule* xr_load_module_compress(XrayIsolate *isolate);

#endif
