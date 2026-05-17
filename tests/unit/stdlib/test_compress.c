/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_compress.c - Unit tests for compression functions
 *
 * KEY CONCEPT:
 *   Tests CRC32/Adler32 checksums, deflate/inflate, gzip/gunzip,
 *   and zlib compress/decompress roundtrips.
 */

#include "../test_framework.h"
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

// Forward declare C-level API from stdlib/compress
typedef enum {
    XR_COMPRESS_OK = 0,
    XR_COMPRESS_ERR_MEMORY,
    XR_COMPRESS_ERR_DATA,
    XR_COMPRESS_ERR_BUFFER,
    XR_COMPRESS_ERR_STREAM,
    XR_COMPRESS_ERR_HEADER,
    XR_COMPRESS_ERR_CHECKSUM
} XrCompressError;

uint32_t xr_crc32(const uint8_t *data, size_t len);
uint32_t xr_crc32_update(uint32_t crc, const uint8_t *data, size_t len);
uint32_t xr_adler32(const uint8_t *data, size_t len);
uint32_t xr_adler32_update(uint32_t adler, const uint8_t *data, size_t len);

XrCompressError xr_deflate(const uint8_t *input, size_t in_len, uint8_t *output, size_t out_cap,
                           size_t *out_len, int level);
XrCompressError xr_inflate(const uint8_t *input, size_t in_len, uint8_t *output, size_t out_cap,
                           size_t *out_len);
size_t xr_deflate_bound(size_t in_len);

uint8_t *xr_gzip_alloc(const uint8_t *input, size_t in_len, size_t *out_len, int level);
uint8_t *xr_gunzip_alloc(const uint8_t *input, size_t in_len, size_t *out_len);
bool xr_is_gzip(const uint8_t *data, size_t len);

const char *xr_compress_error_str(XrCompressError err);

// Need xr_free for heap-allocated versions
#include "base/xmalloc.h"

/* ========== CRC32 ========== */

TEST(compress_crc32_empty) {
    uint32_t crc = xr_crc32((const uint8_t *) "", 0);
    ASSERT_EQ_UINT(crc, 0x00000000);
}

TEST(compress_crc32_known) {
    // CRC32 of "123456789" is 0xCBF43926
    uint32_t crc = xr_crc32((const uint8_t *) "123456789", 9);
    ASSERT_EQ_UINT(crc, 0xCBF43926);
}

TEST(compress_crc32_incremental) {
    // Incremental should match one-shot
    uint32_t crc = xr_crc32_update(0, (const uint8_t *) "1234", 4);
    crc = xr_crc32_update(crc, (const uint8_t *) "56789", 5);
    ASSERT_EQ_UINT(crc, 0xCBF43926);
}

/* ========== Adler32 ========== */

TEST(compress_adler32_empty) {
    uint32_t a = xr_adler32((const uint8_t *) "", 0);
    ASSERT_EQ_UINT(a, 1);  // Adler32 of empty is 1
}

TEST(compress_adler32_known) {
    // Adler32 of "Wikipedia" is 0x11E60398
    uint32_t a = xr_adler32((const uint8_t *) "Wikipedia", 9);
    ASSERT_EQ_UINT(a, 0x11E60398);
}

TEST(compress_adler32_incremental) {
    uint32_t a = xr_adler32_update(1, (const uint8_t *) "Wiki", 4);
    a = xr_adler32_update(a, (const uint8_t *) "pedia", 5);
    ASSERT_EQ_UINT(a, 0x11E60398);
}

/* ========== Deflate / Inflate ========== */

TEST(compress_deflate_inflate_roundtrip) {
    const char *input = "Hello, World! This is a test of deflate compression.";
    size_t in_len = strlen(input);

    size_t bound = xr_deflate_bound(in_len);
    uint8_t *compressed = (uint8_t *) xr_malloc(bound);
    ASSERT_NOT_NULL(compressed);

    size_t comp_len;
    XrCompressError err =
        xr_deflate((const uint8_t *) input, in_len, compressed, bound, &comp_len, 6);
    ASSERT_EQ_INT(err, XR_COMPRESS_OK);
    ASSERT_GT(comp_len, 0);

    // Decompress
    uint8_t *decompressed = (uint8_t *) xr_malloc(in_len + 64);
    size_t decomp_len;
    err = xr_inflate(compressed, comp_len, decompressed, in_len + 64, &decomp_len);
    ASSERT_EQ_INT(err, XR_COMPRESS_OK);
    ASSERT_EQ_UINT(decomp_len, in_len);
    ASSERT_TRUE(memcmp(decompressed, input, in_len) == 0);

    xr_free(compressed);
    xr_free(decompressed);
}

/* ========== Gzip / Gunzip ========== */

TEST(compress_gzip_gunzip_roundtrip) {
    const char *input = "Gzip compression test with some repeated data data data data.";
    size_t in_len = strlen(input);

    size_t comp_len;
    uint8_t *compressed = xr_gzip_alloc((const uint8_t *) input, in_len, &comp_len, 6);
    ASSERT_NOT_NULL(compressed);
    ASSERT_GT(comp_len, 0);

    // Verify gzip header
    ASSERT_TRUE(xr_is_gzip(compressed, comp_len));

    // Decompress
    size_t decomp_len;
    uint8_t *decompressed = xr_gunzip_alloc(compressed, comp_len, &decomp_len);
    ASSERT_NOT_NULL(decompressed);
    ASSERT_EQ_UINT(decomp_len, in_len);
    ASSERT_TRUE(memcmp(decompressed, input, in_len) == 0);

    xr_free(compressed);
    xr_free(decompressed);
}

TEST(compress_is_gzip_invalid) {
    uint8_t not_gzip[] = {0x00, 0x01, 0x02, 0x03};
    ASSERT_FALSE(xr_is_gzip(not_gzip, 4));
    ASSERT_FALSE(xr_is_gzip(NULL, 0));
}

/* ========== Error Strings ========== */

TEST(compress_error_str) {
    ASSERT_NOT_NULL(xr_compress_error_str(XR_COMPRESS_OK));
    ASSERT_NOT_NULL(xr_compress_error_str(XR_COMPRESS_ERR_MEMORY));
    ASSERT_NOT_NULL(xr_compress_error_str(XR_COMPRESS_ERR_DATA));
    ASSERT_NOT_NULL(xr_compress_error_str(XR_COMPRESS_ERR_CHECKSUM));
}

/* ========== Deflate Bound ========== */

TEST(compress_deflate_bound) {
    // Bound should be greater than input for small inputs
    ASSERT_GT(xr_deflate_bound(0), 0);
    ASSERT_GT(xr_deflate_bound(100), 100);
    ASSERT_GT(xr_deflate_bound(1024), 1024);
}

/* ========== Main ========== */

TEST_MAIN_BEGIN()

RUN_TEST_SUITE("Compress - CRC32");
RUN_TEST(compress_crc32_empty);
RUN_TEST(compress_crc32_known);
RUN_TEST(compress_crc32_incremental);

RUN_TEST_SUITE("Compress - Adler32");
RUN_TEST(compress_adler32_empty);
RUN_TEST(compress_adler32_known);
RUN_TEST(compress_adler32_incremental);

RUN_TEST_SUITE("Compress - Deflate / Inflate");
RUN_TEST(compress_deflate_inflate_roundtrip);

RUN_TEST_SUITE("Compress - Gzip / Gunzip");
RUN_TEST(compress_gzip_gunzip_roundtrip);
RUN_TEST(compress_is_gzip_invalid);

RUN_TEST_SUITE("Compress - Utilities");
RUN_TEST(compress_error_str);
RUN_TEST(compress_deflate_bound);

TEST_MAIN_END()
