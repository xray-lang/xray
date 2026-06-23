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

#include "shared/xr_compress_core.h"

// Forward declare C-level API from stdlib/compress
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

static size_t compress_fake_compress_cap;
static int compress_fake_decompress_calls;
static size_t compress_fake_decompress_required;
static size_t compress_fake_decompress_caps[8];

static void *compress_test_alloc(void *ctx, size_t size) {
    (void) ctx;
    return xr_malloc(size);
}

static void compress_test_free(void *ctx, void *ptr) {
    (void) ctx;
    xr_free(ptr);
}

static XrCompressError compress_fake_compress(const uint8_t *input, size_t in_len, uint8_t *output,
                                              size_t out_cap, size_t *out_len, int level) {
    (void) input;
    (void) in_len;
    (void) level;
    compress_fake_compress_cap = out_cap;
    if (!output || out_cap < 4 || !out_len)
        return XR_COMPRESS_ERR_BUFFER;
    memcpy(output, "done", 4);
    *out_len = 4;
    return XR_COMPRESS_OK;
}

static XrCompressError compress_fake_decompress(const uint8_t *input, size_t in_len,
                                                uint8_t *output, size_t out_cap, size_t *out_len) {
    (void) input;
    (void) in_len;
    if (compress_fake_decompress_calls < 8)
        compress_fake_decompress_caps[compress_fake_decompress_calls] = out_cap;
    compress_fake_decompress_calls++;
    if (out_cap < compress_fake_decompress_required)
        return XR_COMPRESS_ERR_BUFFER;
    if (!output || !out_len)
        return XR_COMPRESS_ERR_DATA;
    memcpy(output, "ok", 2);
    *out_len = 2;
    return XR_COMPRESS_OK;
}

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

TEST(compress_inflate_rejects_truncated_stored_block_header) {
    uint8_t truncated[] = {0x01};  // Final stored block tag without LEN/NLEN.
    uint8_t output[16];
    size_t out_len = 0;
    XrCompressError err =
        xr_inflate(truncated, sizeof(truncated), output, sizeof(output), &out_len);
    ASSERT_EQ_INT(err, XR_COMPRESS_ERR_DATA);
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

/* ========== Shared Core Rules ========== */

TEST(compress_core_level_or_default) {
    ASSERT_EQ_INT(xr_compress_core_level_or_default(false, 99), XR_COMPRESS_DEFAULT_COMPRESSION);
    ASSERT_EQ_INT(xr_compress_core_level_or_default(true, -7), XR_COMPRESS_NO_COMPRESSION);
    ASSERT_EQ_INT(xr_compress_core_level_or_default(true, 0), XR_COMPRESS_NO_COMPRESSION);
    ASSERT_EQ_INT(xr_compress_core_level_or_default(true, 5), 5);
    ASSERT_EQ_INT(xr_compress_core_level_or_default(true, 42), XR_COMPRESS_BEST_COMPRESSION);
}

TEST(compress_core_input_view) {
    XrCompressCoreInputView view;
    ASSERT_TRUE(xr_compress_core_input_view("abc", 3, &view));
    ASSERT_EQ_UINT(view.len, 3);
    ASSERT_TRUE(memcmp(view.data, "abc", 3) == 0);

    ASSERT_TRUE(xr_compress_core_input_view(NULL, 0, &view));
    ASSERT_EQ_UINT(view.len, 0);
    ASSERT_NOT_NULL(view.data);

    ASSERT_FALSE(xr_compress_core_input_view(NULL, 1, &view));
    ASSERT_FALSE(xr_compress_core_input_view("abc", -1, &view));
    ASSERT_FALSE(xr_compress_core_input_view("abc", 3, NULL));
}

TEST(compress_core_compress_bound_plans) {
    size_t cap = 0;
    ASSERT_TRUE(xr_compress_core_compress_bound(10, 0, &cap));
    ASSERT_EQ_UINT(cap, xr_deflate_bound(10));

    ASSERT_TRUE(xr_compress_core_compress_bound(10, XR_COMPRESS_CORE_GZIP_WRAPPER_BYTES, &cap));
    ASSERT_EQ_UINT(cap, xr_deflate_bound(10) + XR_COMPRESS_CORE_GZIP_WRAPPER_BYTES);

    ASSERT_TRUE(xr_compress_core_compress_bound(10, XR_COMPRESS_CORE_ZLIB_WRAPPER_BYTES, &cap));
    ASSERT_EQ_UINT(cap, xr_deflate_bound(10) + XR_COMPRESS_CORE_ZLIB_WRAPPER_BYTES);
}

TEST(compress_core_decompress_initial_caps) {
    size_t cap = 0;
    ASSERT_TRUE(xr_compress_core_gunzip_initial_cap(100, 0, &cap));
    ASSERT_EQ_UINT(cap, 100u * XR_COMPRESS_CORE_GUNZIP_FALLBACK_MULTIPLIER +
                            XR_COMPRESS_CORE_GUNZIP_PAD);

    ASSERT_TRUE(xr_compress_core_gunzip_initial_cap(100, 123, &cap));
    ASSERT_EQ_UINT(cap, 123u + XR_COMPRESS_CORE_GUNZIP_PAD);

    ASSERT_TRUE(xr_compress_core_inflate_initial_cap(20, &cap));
    ASSERT_EQ_UINT(cap, 20u * XR_COMPRESS_CORE_INFLATE_FALLBACK_MULTIPLIER +
                            XR_COMPRESS_CORE_INFLATE_PAD);

    ASSERT_TRUE(xr_compress_core_next_cap(32, &cap));
    ASSERT_EQ_UINT(cap, 64);
}

TEST(compress_core_compress_alloc_uses_bound) {
    size_t out_len = 0;
    compress_fake_compress_cap = 0;
    uint8_t *out =
        xr_compress_core_compress_alloc((const uint8_t *) "abc", 3, XR_COMPRESS_DEFAULT_COMPRESSION,
                                        XR_COMPRESS_CORE_GZIP_WRAPPER_BYTES, compress_fake_compress,
                                        compress_test_alloc, compress_test_free, NULL, &out_len);
    ASSERT_NOT_NULL(out);
    ASSERT_EQ_UINT(compress_fake_compress_cap,
                   xr_deflate_bound(3) + XR_COMPRESS_CORE_GZIP_WRAPPER_BYTES);
    ASSERT_EQ_UINT(out_len, 4);
    ASSERT_TRUE(memcmp(out, "done", 4) == 0);
    xr_free(out);
}

TEST(compress_core_decompress_alloc_retries_on_buffer) {
    size_t out_len = 0;
    compress_fake_decompress_calls = 0;
    compress_fake_decompress_required = 40;
    memset(compress_fake_decompress_caps, 0, sizeof(compress_fake_decompress_caps));

    uint8_t *out = xr_compress_core_decompress_alloc((const uint8_t *) "abc", 3, 10, 4,
                                                     compress_fake_decompress, compress_test_alloc,
                                                     compress_test_free, NULL, &out_len);
    ASSERT_NOT_NULL(out);
    ASSERT_EQ_INT(compress_fake_decompress_calls, 3);
    ASSERT_EQ_UINT(compress_fake_decompress_caps[0], 10);
    ASSERT_EQ_UINT(compress_fake_decompress_caps[1], 20);
    ASSERT_EQ_UINT(compress_fake_decompress_caps[2], 40);
    ASSERT_EQ_UINT(out_len, 2);
    ASSERT_TRUE(memcmp(out, "ok", 2) == 0);
    xr_free(out);
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
RUN_TEST(compress_inflate_rejects_truncated_stored_block_header);

RUN_TEST_SUITE("Compress - Gzip / Gunzip");
RUN_TEST(compress_gzip_gunzip_roundtrip);
RUN_TEST(compress_is_gzip_invalid);

RUN_TEST_SUITE("Compress - Utilities");
RUN_TEST(compress_error_str);
RUN_TEST(compress_deflate_bound);
RUN_TEST(compress_core_level_or_default);
RUN_TEST(compress_core_input_view);
RUN_TEST(compress_core_compress_bound_plans);
RUN_TEST(compress_core_decompress_initial_caps);
RUN_TEST(compress_core_compress_alloc_uses_bound);
RUN_TEST(compress_core_decompress_alloc_retries_on_buffer);

TEST_MAIN_END()
