/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * compress_suite_inventory.h - Exact original compression test catalog
 *
 * KEY CONCEPT:
 *   Expected names are independent of catalog discovery. Every original test
 *   body must run; initializing its module cannot substitute for execution.
 */
#ifndef COMPRESS_SUITE_INVENTORY_H
#define COMPRESS_SUITE_INVENTORY_H

#define COMPRESS_SUITE_COUNT 26u
#define COMPRESS_SUITE_SOURCE "tests/regression/10_stdlib/1301_compress.xr"

static const char *const compress_suite_names[COMPRESS_SUITE_COUNT] = {
    "test_crc32",
    "test_crc32_deterministic",
    "test_adler32",
    "test_gzip_roundtrip",
    "test_is_gzip",
    "test_deflate_inflate",
    "test_zlib_roundtrip",
    "test_crc32_known_value",
    "test_crc32_empty",
    "test_adler32_known_value",
    "test_adler32_empty",
    "test_gzip_empty",
    "test_deflate_empty",
    "test_zlib_empty",
    "test_gzip_large_data",
    "test_deflate_large_data",
    "test_gzip_level_zero",
    "test_gzip_all_levels",
    "test_deflate_all_levels",
    "test_zlib_all_levels",
    "test_levels_outside_the_range_are_clamped",
    "test_is_gzip_invalid",
    "test_is_zlib_invalid",
    "test_decompression_refuses_malformed_input",
    "test_truncated_payload_is_refused",
    "test_gzip_repeated_pattern"
};

#endif // COMPRESS_SUITE_INVENTORY_H
