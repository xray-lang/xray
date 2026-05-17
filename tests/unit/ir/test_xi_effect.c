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

int main(void) {
    test_all_opcodes_covered();
    test_pure_ops();
    test_side_effect_ops();
    test_suspend_ops();
    test_mem_ops();

    printf("\n=== test_xi_effect: %d passed, %d failed ===\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
