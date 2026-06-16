/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_opcode.c - Unit tests for opcode properties and names
 */

#include "../test_framework.h"
#include "runtime/value/xchunk.h"
#include <string.h>

/* ========== Opcode Name Tests ========== */

TEST(opcode_name_basic) {
    ASSERT_STR_EQ(xr_opcode_name(OP_MOVE), "MOVE");
    ASSERT_STR_EQ(xr_opcode_name(OP_LOADI), "LOADI");
    ASSERT_STR_EQ(xr_opcode_name(OP_LOADK), "LOADK");
    ASSERT_STR_EQ(xr_opcode_name(OP_LOADNULL), "LOADNULL");
    ASSERT_STR_EQ(xr_opcode_name(OP_LOADTRUE), "LOADTRUE");
    ASSERT_STR_EQ(xr_opcode_name(OP_LOADFALSE), "LOADFALSE");
}

TEST(opcode_name_arithmetic) {
    ASSERT_STR_EQ(xr_opcode_name(OP_ADD), "ADD");
    ASSERT_STR_EQ(xr_opcode_name(OP_SUB), "SUB");
    ASSERT_STR_EQ(xr_opcode_name(OP_MUL), "MUL");
    ASSERT_STR_EQ(xr_opcode_name(OP_DIV), "DIV");
    ASSERT_STR_EQ(xr_opcode_name(OP_MOD), "MOD");
    ASSERT_STR_EQ(xr_opcode_name(OP_UNM), "UNM");
    ASSERT_STR_EQ(xr_opcode_name(OP_NOT), "NOT");
}

TEST(opcode_name_comparison) {
    ASSERT_STR_EQ(xr_opcode_name(OP_EQ), "EQ");
    ASSERT_STR_EQ(xr_opcode_name(OP_LT), "LT");
    ASSERT_STR_EQ(xr_opcode_name(OP_LE), "LE");
    ASSERT_STR_EQ(xr_opcode_name(OP_LTI), "LTI");
    ASSERT_STR_EQ(xr_opcode_name(OP_LEI), "LEI");
}

TEST(opcode_name_control_flow) {
    ASSERT_STR_EQ(xr_opcode_name(OP_JMP), "JMP");
    ASSERT_STR_EQ(xr_opcode_name(OP_CALL), "CALL");
    ASSERT_STR_EQ(xr_opcode_name(OP_RETURN), "RETURN");
    ASSERT_STR_EQ(xr_opcode_name(OP_TAILCALL), "TAILCALL");
}

TEST(opcode_name_container) {
    ASSERT_STR_EQ(xr_opcode_name(OP_NEWARRAY), "NEWARRAY");
    ASSERT_STR_EQ(xr_opcode_name(OP_NEWMAP), "NEWMAP");
    ASSERT_STR_EQ(xr_opcode_name(OP_NEWSET), "NEWSET");
}

TEST(opcode_name_coroutine) {
    ASSERT_STR_EQ(xr_opcode_name(OP_GO), "GO");
    ASSERT_STR_EQ(xr_opcode_name(OP_AWAIT), "AWAIT");
    ASSERT_STR_EQ(xr_opcode_name(OP_YIELD), "YIELD");
}

TEST(opcode_name_channel) {
    ASSERT_STR_EQ(xr_opcode_name(OP_CHAN_NEW), "CHAN_NEW");
    ASSERT_STR_EQ(xr_opcode_name(OP_CHAN_RECV), "CHAN_RECV");
    ASSERT_STR_EQ(xr_opcode_name(OP_CHAN_CLOSE), "CHAN_CLOSE");
}

TEST(opcode_name_nop) {
    ASSERT_STR_EQ(xr_opcode_name(OP_NOP), "NOP");
}

/* ========== Opcode Count Tests ========== */

TEST(opcode_count) {
    // NUM_OPCODES should include all opcodes
    ASSERT_TRUE(NUM_OPCODES > 100);    // Should have many opcodes
    ASSERT_TRUE(NUM_OPCODES < 65536);  // Fits in 16 bits
    ASSERT_EQ_INT(NUM_OPCODES, OP_NOP + 1);
}

TEST(opcode_fits_16bits) {
    // All opcodes should fit in 16 bits
    ASSERT_TRUE(OP_NOP < 65536);
    ASSERT_TRUE(OP_MOVE == 0);  // First opcode
}

/* ========== Instruction Size Constants ========== */

TEST(instruction_word_is_64_bits) {
    ASSERT_EQ_UINT(sizeof(XrInstruction), 8);
}

TEST(instruction_size_constants) {
    ASSERT_EQ_INT(SIZE_OP, 16);
    ASSERT_EQ_INT(SIZE_A, 16);
    ASSERT_EQ_INT(SIZE_B, 16);
    ASSERT_EQ_INT(SIZE_C, 16);
    ASSERT_EQ_INT(SIZE_Bx, 32);
    ASSERT_EQ_INT(SIZE_Ax, 48);
}

TEST(instruction_max_constants) {
    ASSERT_EQ_UINT(MAXARG_A, 65535ull);
    ASSERT_EQ_UINT(MAXARG_B, 65535ull);
    ASSERT_EQ_UINT(MAXARG_C, 65535ull);
    ASSERT_EQ_UINT(MAXARG_Bx, 4294967295ull);
    ASSERT_EQ_INT(MAXARG_sBx, 2147483647);
    ASSERT_EQ_UINT(MAXARG_Ax, 281474976710655ull);
    ASSERT_EQ_UINT(MAXARG_sJ, 140737488355327ull);
}

/* ========== Opcode Category Tests ========== */

TEST(opcode_load_range) {
    // Load instructions should be early opcodes
    ASSERT_TRUE(OP_MOVE < OP_ADD);
    ASSERT_TRUE(OP_LOADI < OP_ADD);
    ASSERT_TRUE(OP_LOADK < OP_ADD);
}

TEST(opcode_arithmetic_grouping) {
    // Arithmetic ops should be grouped together
    ASSERT_TRUE(OP_ADD < OP_SUB);
    ASSERT_TRUE(OP_SUB < OP_MUL);
    ASSERT_TRUE(OP_MUL < OP_DIV);
}

TEST(opcode_bitwise_grouping) {
    // Bitwise ops should be grouped
    ASSERT_TRUE(OP_BAND < OP_BOR);
    ASSERT_TRUE(OP_BOR < OP_BXOR);
    ASSERT_TRUE(OP_BXOR < OP_BNOT);
}

/* ========== Main ========== */

static void run_all_tests(void) {
    RUN_TEST_SUITE("Opcode Names");
    RUN_TEST(opcode_name_basic);
    RUN_TEST(opcode_name_arithmetic);
    RUN_TEST(opcode_name_comparison);
    RUN_TEST(opcode_name_control_flow);
    RUN_TEST(opcode_name_container);
    RUN_TEST(opcode_name_coroutine);
    RUN_TEST(opcode_name_channel);
    RUN_TEST(opcode_name_nop);

    RUN_TEST_SUITE("Opcode Count");
    RUN_TEST(opcode_count);
    RUN_TEST(opcode_fits_16bits);

    RUN_TEST_SUITE("Instruction Constants");
    RUN_TEST(instruction_word_is_64_bits);
    RUN_TEST(instruction_size_constants);
    RUN_TEST(instruction_max_constants);

    RUN_TEST_SUITE("Opcode Ordering");
    RUN_TEST(opcode_load_range);
    RUN_TEST(opcode_arithmetic_grouping);
    RUN_TEST(opcode_bitwise_grouping);
}

TEST_MAIN_BEGIN()
printf("=== xray Opcode Unit Tests ===\n");
run_all_tests();
TEST_MAIN_END()
