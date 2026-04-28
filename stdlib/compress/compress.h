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
 *
 * ====================================================================
 * DESIGN NOTE — this module is the CANONICAL compression entry point.
 * ====================================================================
 *
 * Three separate compression layers exist for historical reasons:
 *
 *   1. stdlib/compress/         — this module. Full-featured gzip +
 *                                 deflate + zlib + CRC32 + Adler32,
 *                                 both caller-buffer and
 *                                 xr_malloc-backed allocating APIs,
 *                                 exposed as the `compress` xray
 *                                 module.
 *
 *   2. stdlib/http/http_compress — HTTP-specific gzip/deflate with
 *                                  Content-Encoding auto-detect and
 *                                  a compressor object pool.
 *
 *   3. stdlib/ws/ws_deflate     — RFC 7692 permessage-deflate with
 *                                  Z_SYNC_FLUSH trailer handling.
 *
 * Consolidation: expose a stateful stream API here, then reduce
 * http_compress and ws_deflate to thin wrappers. New features
 * should go HERE — the other two layers are maintenance-only.
 */

#ifndef XR_STDLIB_COMPRESS_H
#define XR_STDLIB_COMPRESS_H

#include "../../src/base/xdefs.h"

/* ========== Compression Levels ========== */

#define XR_COMPRESS_NO_COMPRESSION 0
#define XR_COMPRESS_BEST_SPEED 1
#define XR_COMPRESS_BEST_COMPRESSION 9
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
XR_FUNC XrCompressError xr_deflate(const uint8_t *input, size_t in_len, uint8_t *output, size_t out_cap,
                           size_t *out_len, int level);

// Raw deflate decompression
XR_FUNC XrCompressError xr_inflate(const uint8_t *input, size_t in_len, uint8_t *output, size_t out_cap,
                           size_t *out_len);

// Estimate maximum compressed size
XR_FUNC size_t xr_deflate_bound(size_t in_len);

/* ========== Gzip Compression/Decompression ========== */

// Gzip compression
XR_FUNC XrCompressError xr_gzip(const uint8_t *input, size_t in_len, uint8_t *output, size_t out_cap,
                        size_t *out_len, int level);

// Gzip decompression
XR_FUNC XrCompressError xr_gunzip(const uint8_t *input, size_t in_len, uint8_t *output, size_t out_cap,
                          size_t *out_len);

// Check if data is valid gzip format
XR_FUNC bool xr_is_gzip(const uint8_t *data, size_t len);

// Get original size from gzip trailer (only lower 32 bits for large files)
XR_FUNC uint32_t xr_gzip_original_size(const uint8_t *data, size_t len);

/* ========== Zlib Format Compression/Decompression ========== */

// Zlib compression (with header and checksum)
XR_FUNC XrCompressError xr_zlib_compress(const uint8_t *input, size_t in_len, uint8_t *output,
                                 size_t out_cap, size_t *out_len, int level);

// Zlib decompression
XR_FUNC XrCompressError xr_zlib_decompress(const uint8_t *input, size_t in_len, uint8_t *output,
                                   size_t out_cap, size_t *out_len);

// Check if data is valid zlib format
XR_FUNC bool xr_is_zlib(const uint8_t *data, size_t len);

/* ========== CRC32 Checksum ========== */

// Compute CRC32 checksum (used by gzip)
XR_FUNC uint32_t xr_crc32(const uint8_t *data, size_t len);

// Incremental CRC32 computation
XR_FUNC uint32_t xr_crc32_update(uint32_t crc, const uint8_t *data, size_t len);

/* ========== Adler32 Checksum ========== */

// Compute Adler32 checksum (used by zlib)
XR_FUNC uint32_t xr_adler32(const uint8_t *data, size_t len);

// Incremental Adler32 computation
XR_FUNC uint32_t xr_adler32_update(uint32_t adler, const uint8_t *data, size_t len);

/* ========== Heap-Allocated Versions ========== */

// Gzip compression with automatic memory allocation
// Returns compressed data (caller must free), NULL on failure
XR_FUNC uint8_t *xr_gzip_alloc(const uint8_t *input, size_t in_len, size_t *out_len, int level);

// Gunzip decompression with automatic memory allocation
// Returns decompressed data (caller must free), NULL on failure
XR_FUNC uint8_t *xr_gunzip_alloc(const uint8_t *input, size_t in_len, size_t *out_len);

/* ========== Error Message ========== */

XR_FUNC const char *xr_compress_error_str(XrCompressError err);

/* ========== Zlib-Backed Stream API (compress_zlib.c) ========== */

/*
 * Stateful stream wrapping system zlib. Provides explicit flush mode
 * control required by WebSocket permessage-deflate (Z_SYNC_FLUSH) and
 * HTTP Transfer-Encoding (Z_FINISH). Routes all heap through
 * xr_malloc/xr_free. See compress_zlib.c for implementation.
 */
typedef struct XrZlibStream XrZlibStream;

// Create a deflate (compress) stream. window_bits:
//   -15        raw deflate (WS permessage-deflate)
//   15         zlib-wrapped
//   16 + 15    gzip-wrapped
XR_FUNC XrZlibStream *xr_zlib_stream_new_deflate(int level, int window_bits);

// Create an inflate (decompress) stream. Same window_bits as above.
XR_FUNC XrZlibStream *xr_zlib_stream_new_inflate(int window_bits);

// Feed input and produce output. flush: Z_NO_FLUSH, Z_SYNC_FLUSH, Z_FINISH.
// Returns: 0 = OK (may need more calls), 1 = stream end, -1 = error.
XR_FUNC int xr_zlib_stream_process(XrZlibStream *s, const uint8_t *in, size_t in_len, uint8_t *out,
                                   size_t out_cap, size_t *out_written, int flush);

// Check if stream has reached Z_STREAM_END.
XR_FUNC bool xr_zlib_stream_finished(const XrZlibStream *s);

// Free the stream (deflateEnd / inflateEnd + xr_free).
XR_FUNC void xr_zlib_stream_free(XrZlibStream *s);

/* --- Convenience: WebSocket permessage-deflate --- */

// Raw deflate + Z_SYNC_FLUSH with trailing 0x00 0x00 0xff 0xff stripped.
// Caller must xr_free(*out).
XR_FUNC int xr_deflate_sync_flush(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len,
                                  int level);

// Raw inflate with RFC 7692 trailer appended + output cap (zip-bomb guard).
// Caller must xr_free(*out).
XR_FUNC int xr_inflate_bounded(const uint8_t *in, size_t in_len, size_t max_out, uint8_t **out,
                               size_t *out_len);

/* --- Convenience: HTTP gzip/deflate --- */

XR_FUNC int xr_zlib_gzip_compress(const void *in, size_t in_len, void **out, size_t *out_len,
                                  int level);
XR_FUNC int xr_zlib_gzip_decompress(const void *in, size_t in_len, void **out, size_t *out_len);
XR_FUNC int xr_zlib_deflate_compress(const void *in, size_t in_len, void **out, size_t *out_len,
                                     int level);
XR_FUNC int xr_zlib_deflate_decompress(const void *in, size_t in_len, void **out, size_t *out_len);

/* --- Content-Encoding detection --- */

typedef enum {
    XR_CONTENT_ENC_NONE = 0,
    XR_CONTENT_ENC_GZIP,
    XR_CONTENT_ENC_DEFLATE
} XrContentEncoding;

XR_FUNC XrContentEncoding xr_detect_content_encoding(const char *encoding);

/* ========== Module Loading ========== */

struct XrayIsolate;
struct XrModule;

XR_FUNC struct XrModule *xr_load_module_compress(struct XrayIsolate *isolate);

#endif  // XR_STDLIB_COMPRESS_H
