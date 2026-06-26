/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_cstr_core.c - Unit tests for runtime-neutral C string argument helpers
 */

#include "../test_framework.h"
#include "shared/xr_cstr_core.h"

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct CStrAllocFake {
    size_t alloc_count;
    size_t last_size;
    bool fail;
} CStrAllocFake;

static void *cstr_core_fake_alloc(void *ctx, size_t size) {
    CStrAllocFake *fake = (CStrAllocFake *) ctx;
    fake->alloc_count++;
    fake->last_size = size;
    if (fake->fail)
        return NULL;
    return malloc(size);
}

TEST(cstr_core_copy_arg_uses_stack_when_it_fits) {
    CStrAllocFake fake = {0};
    char stack[8];
    char *owned = (char *) 0x1;

    char *out =
        xr_cstr_core_copy_arg("abc", 3, stack, sizeof(stack), cstr_core_fake_alloc, &fake, &owned);

    ASSERT_EQ_PTR(out, stack);
    ASSERT_NULL(owned);
    ASSERT_EQ_UINT(fake.alloc_count, 0);
    ASSERT_STR_EQ(out, "abc");
}

TEST(cstr_core_copy_arg_uses_heap_when_stack_is_too_small) {
    CStrAllocFake fake = {0};
    char stack[4];
    char *owned = NULL;

    char *out = xr_cstr_core_copy_arg("abcdef", 6, stack, sizeof(stack), cstr_core_fake_alloc,
                                      &fake, &owned);

    ASSERT_NOT_NULL(out);
    ASSERT_EQ_PTR(out, owned);
    ASSERT_EQ_UINT(fake.alloc_count, 1);
    ASSERT_EQ_UINT(fake.last_size, 7);
    ASSERT_STR_EQ(out, "abcdef");
    free(owned);
}

TEST(cstr_core_copy_arg_allows_exact_stack_capacity_for_nul) {
    CStrAllocFake fake = {0};
    char stack[4];
    char *owned = NULL;

    char *out =
        xr_cstr_core_copy_arg("abc", 3, stack, sizeof(stack), cstr_core_fake_alloc, &fake, &owned);

    ASSERT_EQ_PTR(out, stack);
    ASSERT_NULL(owned);
    ASSERT_EQ_UINT(fake.alloc_count, 0);
    ASSERT_STR_EQ(out, "abc");
}

TEST(cstr_core_copy_arg_rejects_invalid_inputs_and_resets_owned) {
    CStrAllocFake fake = {0};
    char stack[4];
    char *owned = (char *) 0x1;

    ASSERT_NULL(
        xr_cstr_core_copy_arg(NULL, 1, stack, sizeof(stack), cstr_core_fake_alloc, &fake, &owned));
    ASSERT_NULL(owned);
    owned = (char *) 0x1;
    ASSERT_NULL(
        xr_cstr_core_copy_arg("x", -1, stack, sizeof(stack), cstr_core_fake_alloc, &fake, &owned));
    ASSERT_NULL(owned);
    owned = (char *) 0x1;
    ASSERT_NULL(
        xr_cstr_core_copy_arg("x", 1, NULL, sizeof(stack), cstr_core_fake_alloc, &fake, &owned));
    ASSERT_NULL(owned);
    owned = (char *) 0x1;
    ASSERT_NULL(xr_cstr_core_copy_arg("x", 1, stack, 0, cstr_core_fake_alloc, &fake, &owned));
    ASSERT_NULL(owned);
}

TEST(cstr_core_copy_arg_rejects_heap_requirement_without_allocator) {
    char stack[2];
    char *owned = (char *) 0x1;

    ASSERT_NULL(xr_cstr_core_copy_arg("abcd", 4, stack, sizeof(stack), NULL, NULL, &owned));
    ASSERT_NULL(owned);
}

TEST(cstr_core_copy_arg_rejects_allocator_failure) {
    CStrAllocFake fake = {.fail = true};
    char stack[2];
    char *owned = (char *) 0x1;

    ASSERT_NULL(xr_cstr_core_copy_arg("abcd", 4, stack, sizeof(stack), cstr_core_fake_alloc, &fake,
                                      &owned));
    ASSERT_NULL(owned);
    ASSERT_EQ_UINT(fake.alloc_count, 1);
}

TEST(cstr_core_copy_arg_rejects_size_t_overflow) {
    CStrAllocFake fake = {0};
    char stack[2];
    char *owned = (char *) 0x1;

#if SIZE_MAX < INT64_MAX
    ASSERT_NULL(xr_cstr_core_copy_arg("x", INT64_MAX, stack, sizeof(stack), cstr_core_fake_alloc,
                                      &fake, &owned));
    ASSERT_NULL(owned);
    ASSERT_EQ_UINT(fake.alloc_count, 0);
#else
    (void) fake;
    (void) stack;
    (void) owned;
#endif
}

TEST_MAIN_BEGIN()

RUN_TEST_SUITE("C string core");
RUN_TEST(cstr_core_copy_arg_uses_stack_when_it_fits);
RUN_TEST(cstr_core_copy_arg_uses_heap_when_stack_is_too_small);
RUN_TEST(cstr_core_copy_arg_allows_exact_stack_capacity_for_nul);
RUN_TEST(cstr_core_copy_arg_rejects_invalid_inputs_and_resets_owned);
RUN_TEST(cstr_core_copy_arg_rejects_heap_requirement_without_allocator);
RUN_TEST(cstr_core_copy_arg_rejects_allocator_failure);
RUN_TEST(cstr_core_copy_arg_rejects_size_t_overflow);

TEST_MAIN_END()
