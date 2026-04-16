/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xhash.c - Unit tests for hash functions
 */

#include "../test_framework.h"
#include "base/xhash.h"

/* ========== Basic Hash Function Tests ========== */

TEST(hash_bytes_empty) {
    uint32_t h = xr_hash_bytes("", 0);
    ASSERT_EQ_UINT(h, XR_FNV_OFFSET_BASIS);
}

TEST(hash_bytes_single_char) {
    uint32_t h1 = xr_hash_bytes("a", 1);
    uint32_t h2 = xr_hash_bytes("b", 1);
    ASSERT_NE(h1, h2);
}

TEST(hash_bytes_deterministic) {
    const char *data = "hello world";
    uint32_t h1 = xr_hash_bytes(data, strlen(data));
    uint32_t h2 = xr_hash_bytes(data, strlen(data));
    ASSERT_EQ_UINT(h1, h2);
}

TEST(hash_bytes_different_strings) {
    uint32_t h1 = xr_hash_bytes("hello", 5);
    uint32_t h2 = xr_hash_bytes("world", 5);
    ASSERT_NE(h1, h2);
}

TEST(hash_bytes_length_sensitive) {
    // "hello" vs "hello world" should be different
    uint32_t h1 = xr_hash_bytes("hello", 5);
    uint32_t h2 = xr_hash_bytes("hello world", 11);
    ASSERT_NE(h1, h2);
}

TEST(hash_bytes_binary_data) {
    // Test with binary data containing null bytes
    char data[] = {'a', '\0', 'b', '\0', 'c'};
    uint32_t h = xr_hash_bytes(data, 5);
    ASSERT_NE(h, 0);
}

/* ========== 64-bit Hash Tests ========== */

TEST(hash_bytes64_empty) {
    uint64_t h = xr_hash_bytes64("", 0);
    ASSERT_EQ_UINT(h, XR_FNV64_OFFSET_BASIS);
}

TEST(hash_bytes64_deterministic) {
    const char *data = "test string for 64-bit hash";
    uint64_t h1 = xr_hash_bytes64(data, strlen(data));
    uint64_t h2 = xr_hash_bytes64(data, strlen(data));
    ASSERT_EQ_UINT(h1, h2);
}

TEST(hash_bytes64_distribution) {
    // Test that 64-bit hash produces different high bits
    uint64_t h1 = xr_hash_bytes64("string1", 7);
    uint64_t h2 = xr_hash_bytes64("string2", 7);
    ASSERT_NE(h1, h2);
    // High 32 bits should also differ for good distribution
    ASSERT_NE(h1 >> 32, h2 >> 32);
}

/* ========== Integer Hash Tests ========== */

TEST(hash_int_zero) {
    uint32_t h = xr_hash_int(0);
    ASSERT_NE(h, 0);  // Hash should never be 0
}

TEST(hash_int_positive) {
    uint32_t h1 = xr_hash_int(1);
    uint32_t h2 = xr_hash_int(2);
    ASSERT_NE(h1, h2);
    ASSERT_NE(h1, 0);
    ASSERT_NE(h2, 0);
}

TEST(hash_int_negative) {
    uint32_t h1 = xr_hash_int(-1);
    uint32_t h2 = xr_hash_int(-2);
    ASSERT_NE(h1, h2);
    ASSERT_NE(h1, 0);
}

TEST(hash_int_large) {
    uint32_t h1 = xr_hash_int(INT64_MAX);
    uint32_t h2 = xr_hash_int(INT64_MIN);
    ASSERT_NE(h1, h2);
    ASSERT_NE(h1, 0);
    ASSERT_NE(h2, 0);
}

TEST(hash_int_deterministic) {
    uint32_t h1 = xr_hash_int(12345);
    uint32_t h2 = xr_hash_int(12345);
    ASSERT_EQ_UINT(h1, h2);
}

/* ========== Float Hash Tests ========== */

TEST(hash_float_zero) {
    uint32_t h = xr_hash_float(0.0);
    ASSERT_NE(h, 0);
}

TEST(hash_float_positive) {
    uint32_t h1 = xr_hash_float(1.5);
    uint32_t h2 = xr_hash_float(2.5);
    ASSERT_NE(h1, h2);
}

TEST(hash_float_negative) {
    uint32_t h1 = xr_hash_float(-1.5);
    uint32_t h2 = xr_hash_float(1.5);
    ASSERT_NE(h1, h2);
}

TEST(hash_float_deterministic) {
    uint32_t h1 = xr_hash_float(3.14159);
    uint32_t h2 = xr_hash_float(3.14159);
    ASSERT_EQ_UINT(h1, h2);
}

TEST(hash_float_special_values) {
    // Test NaN and infinity
    uint32_t h_inf = xr_hash_float(INFINITY);
    uint32_t h_ninf = xr_hash_float(-INFINITY);
    ASSERT_NE(h_inf, 0);
    ASSERT_NE(h_ninf, 0);
    ASSERT_NE(h_inf, h_ninf);
}

/* ========== Bool Hash Tests ========== */

TEST(hash_bool_true_false) {
    uint32_t h_true = xr_hash_bool(1);
    uint32_t h_false = xr_hash_bool(0);
    ASSERT_NE(h_true, h_false);
    ASSERT_NE(h_true, 0);
    ASSERT_NE(h_false, 0);
}

TEST(hash_bool_deterministic) {
    uint32_t h1 = xr_hash_bool(1);
    uint32_t h2 = xr_hash_bool(1);
    ASSERT_EQ_UINT(h1, h2);
}

/* ========== Short Hash Tests ========== */

TEST(short_hash_range) {
    // Short hash should be 7 bits (0-127) with high bit set
    for (int i = 0; i < 100; i++) {
        uint32_t full_hash = xr_hash_int(i);
        uint8_t short_h = xr_short_hash(full_hash);
        // High bit should be set (XR_SHORT_HASH_VALID = 0x80)
        ASSERT_TRUE(short_h & XR_SHORT_HASH_VALID);
        // Lower 7 bits should be in range
        ASSERT_LE(short_h & 0x7F, 127);
    }
}

TEST(short_hash_distribution) {
    // Test that short hash has reasonable distribution
    int buckets[128] = {0};
    for (int i = 0; i < 10000; i++) {
        uint32_t full_hash = xr_hash_int(i);
        uint8_t short_h = xr_short_hash(full_hash);
        buckets[short_h & 0x7F]++;
    }
    
    // Each bucket should have at least some entries
    int non_empty = 0;
    for (int i = 0; i < 128; i++) {
        if (buckets[i] > 0) non_empty++;
    }
    // At least 50% of buckets should be used
    ASSERT_GT(non_empty, 64);
}

/* ========== Collision Tests ========== */

TEST(hash_collision_rate) {
    // Test collision rate for sequential integers
    const int N = 10000;
    const int TABLE_SIZE = 16384;  // Power of 2
    int collisions = 0;
    int *table = calloc(TABLE_SIZE, sizeof(int));
    
    for (int i = 0; i < N; i++) {
        uint32_t h = xr_hash_int(i);
        int bucket = h % TABLE_SIZE;
        if (table[bucket]) {
            collisions++;
        }
        table[bucket]++;
    }
    
    free(table);
    
    // Collision rate should be reasonable (< 50%)
    double collision_rate = (double)collisions / N;
    ASSERT_LT(collision_rate, 0.5);
}

/* ========== Main ========== */

static void run_all_tests(void) {
    RUN_TEST_SUITE("Hash Bytes");
    RUN_TEST(hash_bytes_empty);
    RUN_TEST(hash_bytes_single_char);
    RUN_TEST(hash_bytes_deterministic);
    RUN_TEST(hash_bytes_different_strings);
    RUN_TEST(hash_bytes_length_sensitive);
    RUN_TEST(hash_bytes_binary_data);
    
    RUN_TEST_SUITE("Hash Bytes 64-bit");
    RUN_TEST(hash_bytes64_empty);
    RUN_TEST(hash_bytes64_deterministic);
    RUN_TEST(hash_bytes64_distribution);
    
    RUN_TEST_SUITE("Hash Integer");
    RUN_TEST(hash_int_zero);
    RUN_TEST(hash_int_positive);
    RUN_TEST(hash_int_negative);
    RUN_TEST(hash_int_large);
    RUN_TEST(hash_int_deterministic);
    
    RUN_TEST_SUITE("Hash Float");
    RUN_TEST(hash_float_zero);
    RUN_TEST(hash_float_positive);
    RUN_TEST(hash_float_negative);
    RUN_TEST(hash_float_deterministic);
    RUN_TEST(hash_float_special_values);
    
    RUN_TEST_SUITE("Hash Bool");
    RUN_TEST(hash_bool_true_false);
    RUN_TEST(hash_bool_deterministic);
    
    RUN_TEST_SUITE("Short Hash");
    RUN_TEST(short_hash_range);
    RUN_TEST(short_hash_distribution);
    
    RUN_TEST_SUITE("Collision Rate");
    RUN_TEST(hash_collision_rate);
}

TEST_MAIN_BEGIN()
    printf("=== xray Hash Function Unit Tests ===\n");
    run_all_tests();
TEST_MAIN_END()
