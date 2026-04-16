/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_framework.h - Minimal unit testing framework for xray
 *
 * KEY CONCEPT:
 *   Simple macros for writing unit tests without external dependencies.
 *   Provides assertion macros, test registration, and result reporting.
 */

#ifndef XR_TEST_FRAMEWORK_H
#define XR_TEST_FRAMEWORK_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

/* ========== Test Counters ========== */

static int xr_tests_run = 0;
static int xr_tests_passed = 0;
static int xr_tests_failed = 0;

/* ========== Test Definition Macros ========== */

#define TEST(name) static void test_##name(void)

#define RUN_TEST(name) do { \
    xr_tests_run++; \
    printf("  %-50s ", #name); \
    fflush(stdout); \
    test_##name(); \
    printf("\033[32mPASS\033[0m\n"); \
    xr_tests_passed++; \
} while(0)

#define RUN_TEST_SUITE(name) do { \
    printf("\n\033[1m[%s]\033[0m\n", name); \
} while(0)

/* ========== Assertion Macros ========== */

#define ASSERT(cond) do { \
    if (!(cond)) { \
        printf("\033[31mFAIL\033[0m\n"); \
        printf("    Assertion failed at %s:%d\n", __FILE__, __LINE__); \
        printf("    Condition: %s\n", #cond); \
        xr_tests_failed++; \
        return; \
    } \
} while(0)

#define ASSERT_MSG(cond, msg) do { \
    if (!(cond)) { \
        printf("\033[31mFAIL\033[0m\n"); \
        printf("    Assertion failed at %s:%d\n", __FILE__, __LINE__); \
        printf("    Message: %s\n", msg); \
        xr_tests_failed++; \
        return; \
    } \
} while(0)

#define ASSERT_EQ(actual, expected) do { \
    if ((actual) != (expected)) { \
        printf("\033[31mFAIL\033[0m\n"); \
        printf("    Assertion failed at %s:%d\n", __FILE__, __LINE__); \
        printf("    Expected: %s == %s\n", #actual, #expected); \
        xr_tests_failed++; \
        return; \
    } \
} while(0)

#define ASSERT_EQ_INT(actual, expected) do { \
    int64_t _a = (int64_t)(actual); \
    int64_t _e = (int64_t)(expected); \
    if (_a != _e) { \
        printf("\033[31mFAIL\033[0m\n"); \
        printf("    Assertion failed at %s:%d\n", __FILE__, __LINE__); \
        printf("    Expected: %lld, Got: %lld\n", (long long)_e, (long long)_a); \
        xr_tests_failed++; \
        return; \
    } \
} while(0)

#define ASSERT_EQ_UINT(actual, expected) do { \
    uint64_t _a = (uint64_t)(actual); \
    uint64_t _e = (uint64_t)(expected); \
    if (_a != _e) { \
        printf("\033[31mFAIL\033[0m\n"); \
        printf("    Assertion failed at %s:%d\n", __FILE__, __LINE__); \
        printf("    Expected: %llu, Got: %llu\n", (unsigned long long)_e, (unsigned long long)_a); \
        xr_tests_failed++; \
        return; \
    } \
} while(0)

#define ASSERT_EQ_PTR(actual, expected) do { \
    const void *_a = (const void *)(actual); \
    const void *_e = (const void *)(expected); \
    if (_a != _e) { \
        printf("\033[31mFAIL\033[0m\n"); \
        printf("    Assertion failed at %s:%d\n", __FILE__, __LINE__); \
        printf("    Expected: %p, Got: %p\n", _e, _a); \
        xr_tests_failed++; \
        return; \
    } \
} while(0)

#define ASSERT_NE(actual, expected) do { \
    if ((actual) == (expected)) { \
        printf("\033[31mFAIL\033[0m\n"); \
        printf("    Assertion failed at %s:%d\n", __FILE__, __LINE__); \
        printf("    Expected: %s != %s\n", #actual, #expected); \
        xr_tests_failed++; \
        return; \
    } \
} while(0)

#define ASSERT_LT(a, b) do { \
    if (!((a) < (b))) { \
        printf("\033[31mFAIL\033[0m\n"); \
        printf("    Assertion failed at %s:%d\n", __FILE__, __LINE__); \
        printf("    Expected: %s < %s\n", #a, #b); \
        xr_tests_failed++; \
        return; \
    } \
} while(0)

#define ASSERT_LE(a, b) do { \
    if (!((a) <= (b))) { \
        printf("\033[31mFAIL\033[0m\n"); \
        printf("    Assertion failed at %s:%d\n", __FILE__, __LINE__); \
        printf("    Expected: %s <= %s\n", #a, #b); \
        xr_tests_failed++; \
        return; \
    } \
} while(0)

#define ASSERT_GT(a, b) do { \
    if (!((a) > (b))) { \
        printf("\033[31mFAIL\033[0m\n"); \
        printf("    Assertion failed at %s:%d\n", __FILE__, __LINE__); \
        printf("    Expected: %s > %s\n", #a, #b); \
        xr_tests_failed++; \
        return; \
    } \
} while(0)

#define ASSERT_GE(a, b) do { \
    if (!((a) >= (b))) { \
        printf("\033[31mFAIL\033[0m\n"); \
        printf("    Assertion failed at %s:%d\n", __FILE__, __LINE__); \
        printf("    Expected: %s >= %s\n", #a, #b); \
        xr_tests_failed++; \
        return; \
    } \
} while(0)

#define ASSERT_NULL(ptr) do { \
    if ((ptr) != NULL) { \
        printf("\033[31mFAIL\033[0m\n"); \
        printf("    Assertion failed at %s:%d\n", __FILE__, __LINE__); \
        printf("    Expected NULL, got: %p\n", (void*)(ptr)); \
        xr_tests_failed++; \
        return; \
    } \
} while(0)

#define ASSERT_NOT_NULL(ptr) do { \
    if ((ptr) == NULL) { \
        printf("\033[31mFAIL\033[0m\n"); \
        printf("    Assertion failed at %s:%d\n", __FILE__, __LINE__); \
        printf("    Expected non-NULL pointer\n"); \
        xr_tests_failed++; \
        return; \
    } \
} while(0)

#define ASSERT_STR_EQ(actual, expected) do { \
    const char *_a = (actual); \
    const char *_e = (expected); \
    if (_a == NULL || _e == NULL || strcmp(_a, _e) != 0) { \
        printf("\033[31mFAIL\033[0m\n"); \
        printf("    Assertion failed at %s:%d\n", __FILE__, __LINE__); \
        printf("    Expected: \"%s\"\n", _e ? _e : "(null)"); \
        printf("    Got:      \"%s\"\n", _a ? _a : "(null)"); \
        xr_tests_failed++; \
        return; \
    } \
} while(0)

#define ASSERT_STR_NE(actual, expected) do { \
    const char *_a = (actual); \
    const char *_e = (expected); \
    if (_a != NULL && _e != NULL && strcmp(_a, _e) == 0) { \
        printf("\033[31mFAIL\033[0m\n"); \
        printf("    Assertion failed at %s:%d\n", __FILE__, __LINE__); \
        printf("    Strings should not be equal: \"%s\"\n", _a); \
        xr_tests_failed++; \
        return; \
    } \
} while(0)

#define ASSERT_MEM_EQ(actual, expected, size) do { \
    if (memcmp((actual), (expected), (size)) != 0) { \
        printf("\033[31mFAIL\033[0m\n"); \
        printf("    Assertion failed at %s:%d\n", __FILE__, __LINE__); \
        printf("    Memory blocks not equal (size: %zu)\n", (size_t)(size)); \
        xr_tests_failed++; \
        return; \
    } \
} while(0)

#define ASSERT_FLOAT_EQ(actual, expected, epsilon) do { \
    double _a = (double)(actual); \
    double _e = (double)(expected); \
    double _eps = (double)(epsilon); \
    if (fabs(_a - _e) > _eps) { \
        printf("\033[31mFAIL\033[0m\n"); \
        printf("    Assertion failed at %s:%d\n", __FILE__, __LINE__); \
        printf("    Expected: %g (±%g), Got: %g\n", _e, _eps, _a); \
        xr_tests_failed++; \
        return; \
    } \
} while(0)

#define ASSERT_TRUE(cond)  ASSERT(cond)
#define ASSERT_FALSE(cond) ASSERT(!(cond))

/* ========== Test Result Reporting ========== */

#define TEST_REPORT() do { \
    printf("\n"); \
    printf("========================================\n"); \
    if (xr_tests_failed == 0) { \
        printf("\033[32m✓ All %d tests passed\033[0m\n", xr_tests_passed); \
    } else { \
        printf("\033[31m✗ %d/%d tests failed\033[0m\n", \
               xr_tests_failed, xr_tests_run); \
    } \
    printf("========================================\n"); \
} while(0)

#define TEST_EXIT() (xr_tests_failed > 0 ? 1 : 0)

/* ========== Main Function Helper ========== */

#define TEST_MAIN_BEGIN() \
int main(int argc, char **argv) { \
    (void)argc; (void)argv; \
    printf("\n");

#define TEST_MAIN_END() \
    TEST_REPORT(); \
    return TEST_EXIT(); \
}

#endif // XR_TEST_FRAMEWORK_H
