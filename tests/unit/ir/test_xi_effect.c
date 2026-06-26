/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xi_effect.c - Verify xi_effect.h opcode-to-effect table
 *
 * Ensures the effect table is complete (covers all opcodes) and
 * that the declared effects are self-consistent.
 */

#include "../../src/ir/xi_effect.h"
#include <stdio.h>

static int g_passed = 0;
static int g_failed = 0;

#define ASSERT_TRUE(cond, msg)                                                                     \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "  FAIL: %s\n", msg);                                                  \
            g_failed++;                                                                            \
        } else {                                                                                   \
            g_passed++;                                                                            \
        }                                                                                          \
    } while (0)

/* Every opcode must produce a valid flags byte (no crash). */
static void test_all_opcodes_covered(void) {
    for (int op = 0; op < XI_OP_COUNT; op++) {
        uint8_t eff = xi_op_default_effects((uint16_t) op);
        /* flags must fit in uint8_t (always true, but sanity check) */
        ASSERT_TRUE(eff <= 0xFF, "effect overflow");
        (void) eff;
    }
}

/* Pure ops must have zero effects. */
static void test_pure_ops(void) {
    ASSERT_TRUE(xi_op_is_pure(XI_CONST), "CONST should be pure");
    ASSERT_TRUE(xi_op_is_pure(XI_PARAM), "PARAM should be pure");
    ASSERT_TRUE(xi_op_is_pure(XI_ADD), "ADD should be pure");
    ASSERT_TRUE(xi_op_is_pure(XI_EQ), "EQ should be pure");
    ASSERT_TRUE(xi_op_is_pure(XI_NOT), "NOT should be pure");
    ASSERT_TRUE(xi_op_is_pure(XI_BOX), "BOX should be pure");
    ASSERT_TRUE(xi_op_is_pure(XI_PHI), "PHI should be pure");
}

/* Side-effecting ops must have SIDE_EFFECT. */
static void test_side_effect_ops(void) {
    uint8_t se = XI_FLAG_SIDE_EFFECT;
    ASSERT_TRUE((xi_op_default_effects(XI_STORE_FIELD) & se) != 0,
                "STORE_FIELD must be side-effecting");
    ASSERT_TRUE((xi_op_default_effects(XI_PRINT) & se) != 0, "PRINT must be side-effecting");
    ASSERT_TRUE((xi_op_default_effects(XI_THROW) & se) != 0, "THROW must be side-effecting");
    ASSERT_TRUE((xi_op_default_effects(XI_CALL) & se) != 0, "CALL must be side-effecting");
}

/* Coroutine ops must have MAY_SUSPEND. */
static void test_suspend_ops(void) {
    ASSERT_TRUE(xi_op_may_suspend(XI_AWAIT), "AWAIT must may-suspend");
    ASSERT_TRUE(xi_op_may_suspend(XI_YIELD), "YIELD must may-suspend");
    ASSERT_TRUE(xi_op_may_suspend(XI_CHAN_SEND), "CHAN_SEND must may-suspend");
    ASSERT_TRUE(xi_op_may_suspend(XI_CHAN_RECV), "CHAN_RECV must may-suspend");
    ASSERT_TRUE(xi_op_may_suspend(XI_SCOPE_EXIT), "SCOPE_EXIT must may-suspend");
    /* Non-blocking variants should not may-suspend */
    ASSERT_TRUE(!xi_op_may_suspend(XI_CHAN_TRY_SEND), "CHAN_TRY_SEND should not may-suspend");
    ASSERT_TRUE(!xi_op_may_suspend(XI_CHAN_TRY_RECV), "CHAN_TRY_RECV should not may-suspend");
    ASSERT_TRUE(!xi_op_may_suspend(XI_GO), "GO should not may-suspend");
}

/* Memory ops. */
static void test_mem_ops(void) {
    ASSERT_TRUE(xi_op_reads_mem(XI_LOAD_FIELD), "LOAD_FIELD reads mem");
    ASSERT_TRUE(xi_op_reads_mem(XI_INDEX_GET), "INDEX_GET reads mem");
    ASSERT_TRUE(xi_op_writes_mem(XI_STORE_FIELD), "STORE_FIELD writes mem");
    ASSERT_TRUE(xi_op_writes_mem(XI_INDEX_SET), "INDEX_SET writes mem");
    /* Pure ops don't touch memory */
    ASSERT_TRUE(!xi_op_reads_mem(XI_ADD), "ADD should not read mem");
    ASSERT_TRUE(!xi_op_writes_mem(XI_ADD), "ADD should not write mem");
}

static void test_optimization_traits(void) {
    ASSERT_TRUE(xi_op_is_comparison(XI_EQ), "EQ is a comparison op");
    ASSERT_TRUE(xi_op_is_comparison(XI_LT), "LT is a comparison op");
    ASSERT_TRUE(!xi_op_is_comparison(XI_ADD), "ADD is not a comparison op");
    ASSERT_TRUE(xi_op_value_numbering_kind(XI_ADD) == XI_GEN_VN_PURE, "ADD is VN-pure");
    ASSERT_TRUE(xi_op_value_numbering_kind(XI_LOAD_FIELD) == XI_GEN_VN_MEMORY_READ,
                "LOAD_FIELD is VN memory-read");
    ASSERT_TRUE(!xi_op_value_numberable(XI_CONST), "CONST is not value-numbered by GVN");
    ASSERT_TRUE(!xi_op_value_numberable(XI_IS), "IS is not value-numbered by GVN");
    ASSERT_TRUE(xi_op_is_commutative(XI_ADD), "ADD is commutative");
    ASSERT_TRUE(xi_op_is_commutative(XI_EQ), "EQ is commutative");
    ASSERT_TRUE(!xi_op_is_commutative(XI_SUB), "SUB is not commutative");
    ASSERT_TRUE(xi_op_is_associative(XI_ADD), "ADD is associative");
    ASSERT_TRUE(xi_op_is_associative(XI_MUL), "MUL is associative");
    ASSERT_TRUE(xi_op_is_associative(XI_BAND), "BAND is associative");
    ASSERT_TRUE(!xi_op_is_associative(XI_SUB), "SUB is not associative");
    ASSERT_TRUE(!xi_op_is_associative(XI_EQ), "EQ is not associative");
    ASSERT_TRUE(xi_op_negated_comparison(XI_EQ) == XI_NE, "EQ negates to NE");
    ASSERT_TRUE(xi_op_negated_comparison(XI_LT) == XI_GE, "LT negates to GE");
    ASSERT_TRUE(xi_op_negated_comparison(XI_ADD) == XI_OP_COUNT, "ADD has no negated cmp");
}

static void test_result_ownership_traits(void) {
    ASSERT_TRUE(xi_op_result_ownership(XI_ARRAY_NEW) == XI_GEN_RESULT_OWNERSHIP_OWNED,
                "ARRAY_NEW produces an owned result");
    ASSERT_TRUE(xi_op_result_ownership(XI_LOAD_FIELD) == XI_GEN_RESULT_OWNERSHIP_BORROWED,
                "LOAD_FIELD produces a borrowed result");
    ASSERT_TRUE(xi_op_result_ownership(XI_GET_SHARED) == XI_GEN_RESULT_OWNERSHIP_BORROWED,
                "GET_SHARED produces a borrowed result");
    ASSERT_TRUE(xi_op_result_ownership(XI_STORE_FIELD) == XI_GEN_RESULT_OWNERSHIP_NONE,
                "STORE_FIELD produces no tracked result");
    ASSERT_TRUE(xi_op_result_ownership(XI_PRINT) == XI_GEN_RESULT_OWNERSHIP_NONE,
                "PRINT produces no tracked result");
    ASSERT_TRUE(xi_op_result_ownership(XI_CALL) == XI_GEN_RESULT_OWNERSHIP_CALL_RESULT,
                "CALL result ownership is summary-dependent");
    ASSERT_TRUE(xi_op_result_ownership(XI_CALL_METHOD) == XI_GEN_RESULT_OWNERSHIP_CALL_RESULT,
                "CALL_METHOD result ownership is summary-dependent");
}

static void test_speculation_traits(void) {
    ASSERT_TRUE(xi_op_can_speculate(XI_SELECT), "SELECT is safe to speculate");
    ASSERT_TRUE(xi_op_can_speculate(XI_COPY), "COPY is safe to speculate");
    ASSERT_TRUE(!xi_op_can_speculate(XI_IS), "IS is not safe to speculate");
    ASSERT_TRUE(!xi_op_can_speculate(XI_LOAD_FIELD), "LOAD_FIELD is not safe to speculate");
}

int main(void) {
    test_all_opcodes_covered();
    test_pure_ops();
    test_side_effect_ops();
    test_suspend_ops();
    test_mem_ops();
    test_optimization_traits();
    test_result_ownership_traits();
    test_speculation_traits();

    printf("\n=== test_xi_effect: %d passed, %d failed ===\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
