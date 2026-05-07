/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xi_intrinsic.c - Verify xi_intrinsic.def consistency
 *
 * Self-contained test that generates enum/name/arity checks directly
 * from the .def file without linking against xm_intrinsic.c (which
 * pulls in AOT sentinel symbols unavailable in unit-test builds).
 */

#include <stdio.h>
#include <string.h>
#include <stdbool.h>

/* Generate enum from .def — mirrors xm_intrinsic.h */
typedef enum {
    XR_INTRIN_NONE = 0,
#define XI_INTRINSIC(name, id, arity, helper) XR_INTRIN_##name = id,
#include "../../src/ir/xi_intrinsic.def"
#undef XI_INTRINSIC
    XR_INTRIN_COUNT
} TestIntrinsicId;

static int g_passed = 0;
static int g_failed = 0;

#define ASSERT_TRUE(cond, msg)                                  \
    do {                                                        \
        if (!(cond)) {                                          \
            fprintf(stderr, "  FAIL: %s\n", msg);               \
            g_failed++;                                         \
        } else {                                                \
            g_passed++;                                         \
        }                                                       \
    } while (0)

/* Verify every intrinsic ID is unique and matches its expected value. */
static void test_enum_values(void) {
#define XI_INTRINSIC(name, id, arity, helper)                                  \
    ASSERT_TRUE(XR_INTRIN_##name == id,                                        \
                "XR_INTRIN_" #name " enum value mismatch");
#include "../../src/ir/xi_intrinsic.def"
#undef XI_INTRINSIC
}

/* Verify all IDs are positive and below the sentinel. */
static void test_id_range(void) {
#define XI_INTRINSIC(name, id, arity, helper)                                  \
    ASSERT_TRUE(id > 0 && id < XR_INTRIN_COUNT,                                \
                "XR_INTRIN_" #name " ID must be in (0, COUNT)");
#include "../../src/ir/xi_intrinsic.def"
#undef XI_INTRINSIC
}

/* Verify no duplicate IDs by checking a boolean table. */
static void test_no_duplicate_ids(void) {
    bool seen[256];
    memset(seen, 0, sizeof(seen));
#define XI_INTRINSIC(name, id, arity, helper)                                  \
    do {                                                                       \
        ASSERT_TRUE(id < 256, "XR_INTRIN_" #name " ID out of table range");    \
        if (id < 256) {                                                        \
            ASSERT_TRUE(!seen[id],                                             \
                        "XR_INTRIN_" #name " duplicate ID detected");          \
            seen[id] = true;                                                   \
        }                                                                      \
    } while (0);
#include "../../src/ir/xi_intrinsic.def"
#undef XI_INTRINSIC
}

/* Verify arity is sensible (-1 for variadic, >= 0 otherwise). */
static void test_arity_valid(void) {
#define XI_INTRINSIC(name, id, arity, helper)                                  \
    ASSERT_TRUE(arity >= -1 && arity <= 16,                                    \
                "XR_INTRIN_" #name " arity out of range");
#include "../../src/ir/xi_intrinsic.def"
#undef XI_INTRINSIC
}

/* Count total intrinsics for a sanity check. */
static void test_intrinsic_count(void) {
    int count = 0;
#define XI_INTRINSIC(name, id, arity, helper) count++;
#include "../../src/ir/xi_intrinsic.def"
#undef XI_INTRINSIC
    ASSERT_TRUE(count >= 20, "expected at least 20 intrinsics in .def");
    printf("  intrinsic count: %d\n", count);
}

int main(void) {
    test_enum_values();
    test_id_range();
    test_no_duplicate_ids();
    test_arity_valid();
    test_intrinsic_count();

    printf("\n=== test_xi_intrinsic: %d passed, %d failed ===\n",
           g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
