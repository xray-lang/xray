/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xi_intrinsic.c - Verify xi_intrinsic.def and xi_method_sym.def
 *
 * Self-contained test that generates enum/name/arity checks directly
 * from the .def files without linking against xm_intrinsic.c (which
 * pulls in AOT sentinel symbols unavailable in unit-test builds).
 */

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "../../src/ir/xi_intrinsic_flags.h"

/* Generate enum from xi_intrinsic.def — mirrors xm_intrinsic.h */
typedef enum {
    XR_INTRIN_NONE = 0,
#define XI_INTRINSIC(name, id, arity, helper, eff, rep) XR_INTRIN_##name = id,
#include "../../src/ir/xi_intrinsic.def"
#undef XI_INTRINSIC
    XR_INTRIN_COUNT
} TestIntrinsicId;

/* Generate enum from xi_method_sym.def — mirrors xrt_method_symbols.h */
enum {
#define XI_METHOD_SYM(aot_name, id, rt_name, display_name) \
    TEST_SYM_##aot_name = id,
#include "../../src/ir/xi_method_sym.def"
#undef XI_METHOD_SYM
    TEST_SYM_COUNT_
};

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

/* ========== xi_intrinsic.def tests ========== */

/* Verify every intrinsic ID is unique and matches its expected value. */
static void test_enum_values(void) {
#define XI_INTRINSIC(name, id, arity, helper, eff, rep)                        \
    ASSERT_TRUE(XR_INTRIN_##name == id,                                        \
                "XR_INTRIN_" #name " enum value mismatch");
#include "../../src/ir/xi_intrinsic.def"
#undef XI_INTRINSIC
}

/* Verify all IDs are positive and below the sentinel. */
static void test_id_range(void) {
#define XI_INTRINSIC(name, id, arity, helper, eff, rep)                        \
    ASSERT_TRUE(id > 0 && id < XR_INTRIN_COUNT,                                \
                "XR_INTRIN_" #name " ID must be in (0, COUNT)");
#include "../../src/ir/xi_intrinsic.def"
#undef XI_INTRINSIC
}

/* Verify no duplicate IDs by checking a boolean table. */
static void test_no_duplicate_ids(void) {
    bool seen[256];
    memset(seen, 0, sizeof(seen));
#define XI_INTRINSIC(name, id, arity, helper, eff, rep)                        \
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
#define XI_INTRINSIC(name, id, arity, helper, eff, rep)                        \
    ASSERT_TRUE(arity >= -1 && arity <= 16,                                    \
                "XR_INTRIN_" #name " arity out of range");
#include "../../src/ir/xi_intrinsic.def"
#undef XI_INTRINSIC
}

/* Count total intrinsics for a sanity check. */
static void test_intrinsic_count(void) {
    int count = 0;
#define XI_INTRINSIC(name, id, arity, helper, eff, rep) count++;
#include "../../src/ir/xi_intrinsic.def"
#undef XI_INTRINSIC
    ASSERT_TRUE(count >= 20, "expected at least 20 intrinsics in .def");
    printf("  intrinsic count: %d\n", count);
}

/* Verify effect flags are valid (no undefined bits). */
static void test_effect_flags(void) {
    const int VALID_MASK = IEFF_R | IEFF_W | IEFF_T | IEFF_IO | IEFF_A;
#define XI_INTRINSIC(name, id, arity, helper, eff, rep)                        \
    ASSERT_TRUE(((eff) & ~VALID_MASK) == 0,                                    \
                "XR_INTRIN_" #name " has invalid effect bits");
#include "../../src/ir/xi_intrinsic.def"
#undef XI_INTRINSIC
}

/* Verify return reps are valid. */
static void test_ret_rep_valid(void) {
#define XI_INTRINSIC(name, id, arity, helper, eff, rep)                        \
    ASSERT_TRUE((rep) >= IREP_VAL && (rep) <= IREP_I64,                        \
                "XR_INTRIN_" #name " invalid ret_rep");
#include "../../src/ir/xi_intrinsic.def"
#undef XI_INTRINSIC
}

/* ========== xi_method_sym.def tests ========== */

/* Verify method symbol IDs are positive. */
static void test_method_sym_ids(void) {
#define XI_METHOD_SYM(aot_name, id, rt_name, display_name) \
    ASSERT_TRUE(TEST_SYM_##aot_name == id,                 \
                "TEST_SYM_" #aot_name " value mismatch");  \
    ASSERT_TRUE(id > 0,                                    \
                "TEST_SYM_" #aot_name " ID must be positive");
#include "../../src/ir/xi_method_sym.def"
#undef XI_METHOD_SYM
}

/* Verify no duplicate method symbol IDs. */
static void test_method_sym_no_dups(void) {
    bool seen[256];
    memset(seen, 0, sizeof(seen));
#define XI_METHOD_SYM(aot_name, id, rt_name, display_name) \
    do {                                                    \
        if (id < 256) {                                     \
            ASSERT_TRUE(!seen[id],                          \
                "TEST_SYM_" #aot_name " duplicate ID");     \
            seen[id] = true;                                \
        }                                                   \
    } while (0);
#include "../../src/ir/xi_method_sym.def"
#undef XI_METHOD_SYM
}

/* Verify display names are non-NULL and non-empty. */
static void test_method_sym_names(void) {
#define XI_METHOD_SYM(aot_name, id, rt_name, display_name) \
    ASSERT_TRUE(display_name != NULL && display_name[0] != '\0', \
                "TEST_SYM_" #aot_name " display name empty");
#include "../../src/ir/xi_method_sym.def"
#undef XI_METHOD_SYM
}

/* Count method symbols. */
static void test_method_sym_count(void) {
    int count = 0;
#define XI_METHOD_SYM(aot_name, id, rt_name, display_name) count++;
#include "../../src/ir/xi_method_sym.def"
#undef XI_METHOD_SYM
    ASSERT_TRUE(count >= 50, "expected at least 50 method symbols in .def");
    printf("  method symbol count: %d\n", count);
}

int main(void) {
    printf("--- xi_intrinsic.def ---\n");
    test_enum_values();
    test_id_range();
    test_no_duplicate_ids();
    test_arity_valid();
    test_intrinsic_count();
    test_effect_flags();
    test_ret_rep_valid();

    printf("--- xi_method_sym.def ---\n");
    test_method_sym_ids();
    test_method_sym_no_dups();
    test_method_sym_names();
    test_method_sym_count();

    printf("\n=== test_xi_intrinsic: %d passed, %d failed ===\n",
           g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
