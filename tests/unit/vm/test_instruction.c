/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_instruction.c - Unit tests for bytecode instruction encoding/decoding
 */

#include "../test_framework.h"
#include "runtime/value/xchunk.h"

/* ========== Basic Encoding Tests ========== */

TEST(instruction_create_abc) {
    XrInstruction i = CREATE_ABC(OP_ADD, 1, 2, 3);
    
    ASSERT_EQ_INT(GET_OPCODE(i), OP_ADD);
    ASSERT_EQ_INT(GETARG_A(i), 1);
    ASSERT_EQ_INT(GETARG_B(i), 2);
    ASSERT_EQ_INT(GETARG_C(i), 3);
}

TEST(instruction_create_abc_max_values) {
    XrInstruction i = CREATE_ABC(OP_MOVE, 255, 255, 255);
    
    ASSERT_EQ_INT(GET_OPCODE(i), OP_MOVE);
    ASSERT_EQ_INT(GETARG_A(i), 255);
    ASSERT_EQ_INT(GETARG_B(i), 255);
    ASSERT_EQ_INT(GETARG_C(i), 255);
}

TEST(instruction_create_abc_zero) {
    XrInstruction i = CREATE_ABC(OP_LOADNULL, 0, 0, 0);
    
    ASSERT_EQ_INT(GET_OPCODE(i), OP_LOADNULL);
    ASSERT_EQ_INT(GETARG_A(i), 0);
    ASSERT_EQ_INT(GETARG_B(i), 0);
    ASSERT_EQ_INT(GETARG_C(i), 0);
}

/* ========== ABx Format Tests ========== */

TEST(instruction_create_abx) {
    XrInstruction i = CREATE_ABx(OP_LOADK, 5, 1000);
    
    ASSERT_EQ_INT(GET_OPCODE(i), OP_LOADK);
    ASSERT_EQ_INT(GETARG_A(i), 5);
    ASSERT_EQ_INT(GETARG_Bx(i), 1000);
}

TEST(instruction_create_abx_max) {
    XrInstruction i = CREATE_ABx(OP_LOADK, 255, 65535);
    
    ASSERT_EQ_INT(GET_OPCODE(i), OP_LOADK);
    ASSERT_EQ_INT(GETARG_A(i), 255);
    ASSERT_EQ_INT(GETARG_Bx(i), 65535);
}

/* ========== AsBx Format Tests (Signed) ========== */

TEST(instruction_create_asbx_positive) {
    XrInstruction i = CREATE_AsBx(OP_LOADI, 10, 100);
    
    ASSERT_EQ_INT(GET_OPCODE(i), OP_LOADI);
    ASSERT_EQ_INT(GETARG_A(i), 10);
    ASSERT_EQ_INT(GETARG_sBx(i), 100);
}

TEST(instruction_create_asbx_negative) {
    XrInstruction i = CREATE_AsBx(OP_LOADI, 10, -100);
    
    ASSERT_EQ_INT(GET_OPCODE(i), OP_LOADI);
    ASSERT_EQ_INT(GETARG_A(i), 10);
    ASSERT_EQ_INT(GETARG_sBx(i), -100);
}

TEST(instruction_create_asbx_zero) {
    XrInstruction i = CREATE_AsBx(OP_LOADI, 5, 0);
    
    ASSERT_EQ_INT(GET_OPCODE(i), OP_LOADI);
    ASSERT_EQ_INT(GETARG_A(i), 5);
    ASSERT_EQ_INT(GETARG_sBx(i), 0);
}

TEST(instruction_create_asbx_max) {
    XrInstruction i = CREATE_AsBx(OP_LOADI, 0, MAXARG_sBx);
    
    ASSERT_EQ_INT(GET_OPCODE(i), OP_LOADI);
    ASSERT_EQ_INT(GETARG_sBx(i), MAXARG_sBx);
}

TEST(instruction_create_asbx_min) {
    XrInstruction i = CREATE_AsBx(OP_LOADI, 0, -MAXARG_sBx);
    
    ASSERT_EQ_INT(GET_OPCODE(i), OP_LOADI);
    ASSERT_EQ_INT(GETARG_sBx(i), -MAXARG_sBx);
}

/* ========== Ax Format Tests ========== */

TEST(instruction_create_ax) {
    XrInstruction i = CREATE_Ax(OP_CLOSURE, 12345);
    
    ASSERT_EQ_INT(GET_OPCODE(i), OP_CLOSURE);
    ASSERT_EQ_INT(GETARG_Ax(i), 12345);
}

TEST(instruction_create_ax_max) {
    XrInstruction i = CREATE_Ax(OP_CLOSURE, MAXARG_Ax);
    
    ASSERT_EQ_INT(GET_OPCODE(i), OP_CLOSURE);
    ASSERT_EQ_INT(GETARG_Ax(i), MAXARG_Ax);
}

/* ========== sJ Format Tests (Signed Jump) ========== */

TEST(instruction_create_sj_positive) {
    XrInstruction i = CREATE_sJ(OP_JMP, 50);
    
    ASSERT_EQ_INT(GET_OPCODE(i), OP_JMP);
    ASSERT_EQ_INT(GETARG_sJ(i), 50);
}

TEST(instruction_create_sj_negative) {
    XrInstruction i = CREATE_sJ(OP_JMP, -50);
    
    ASSERT_EQ_INT(GET_OPCODE(i), OP_JMP);
    ASSERT_EQ_INT(GETARG_sJ(i), -50);
}

TEST(instruction_create_sj_zero) {
    XrInstruction i = CREATE_sJ(OP_JMP, 0);
    
    ASSERT_EQ_INT(GET_OPCODE(i), OP_JMP);
    ASSERT_EQ_INT(GETARG_sJ(i), 0);
}

/* ========== Argument Modification Tests ========== */

TEST(instruction_setarg_a) {
    XrInstruction i = CREATE_ABC(OP_ADD, 1, 2, 3);
    SETARG_A(i, 100);
    
    ASSERT_EQ_INT(GETARG_A(i), 100);
    ASSERT_EQ_INT(GETARG_B(i), 2);  // unchanged
    ASSERT_EQ_INT(GETARG_C(i), 3);  // unchanged
}

TEST(instruction_setarg_b) {
    XrInstruction i = CREATE_ABC(OP_ADD, 1, 2, 3);
    SETARG_B(i, 200);
    
    ASSERT_EQ_INT(GETARG_A(i), 1);  // unchanged
    ASSERT_EQ_INT(GETARG_B(i), 200);
    ASSERT_EQ_INT(GETARG_C(i), 3);  // unchanged
}

TEST(instruction_setarg_c) {
    XrInstruction i = CREATE_ABC(OP_ADD, 1, 2, 3);
    SETARG_C(i, 150);
    
    ASSERT_EQ_INT(GETARG_A(i), 1);  // unchanged
    ASSERT_EQ_INT(GETARG_B(i), 2);  // unchanged
    ASSERT_EQ_INT(GETARG_C(i), 150);
}

TEST(instruction_setarg_bx) {
    XrInstruction i = CREATE_ABx(OP_LOADK, 5, 100);
    SETARG_Bx(i, 50000);
    
    ASSERT_EQ_INT(GETARG_A(i), 5);  // unchanged
    ASSERT_EQ_INT(GETARG_Bx(i), 50000);
}

/* ========== Signed Argument Tests ========== */

TEST(instruction_signed_b) {
    XrInstruction i = CREATE_ABC(OP_ADDI, 0, 0, 200);  // 200 as unsigned
    
    // sC interprets as signed: 200 = -56 in signed 8-bit
    int8_t sC = GETARG_sC(i);
    ASSERT_EQ_INT(sC, -56);
}

TEST(instruction_signed_b_positive) {
    XrInstruction i = CREATE_ABC(OP_ADDI, 0, 0, 50);
    
    int8_t sC = GETARG_sC(i);
    ASSERT_EQ_INT(sC, 50);
}

/* ========== Opcode Range Tests ========== */

TEST(instruction_opcodes_fit_8bits) {
    // All opcodes should fit in 8 bits
    ASSERT_TRUE(NUM_OPCODES <= 256);
}

TEST(instruction_opcode_nop_is_last) {
    // OP_NOP should be the last opcode
    ASSERT_EQ_INT(OP_NOP, NUM_OPCODES - 1);
}

/* ========== Specific Instruction Tests ========== */

TEST(instruction_move) {
    XrInstruction i = CREATE_ABC(OP_MOVE, 5, 10, 0);
    
    ASSERT_EQ_INT(GET_OPCODE(i), OP_MOVE);
    ASSERT_EQ_INT(GETARG_A(i), 5);   // dest register
    ASSERT_EQ_INT(GETARG_B(i), 10);  // source register
}

TEST(instruction_arithmetic) {
    // ADD R[0] = R[1] + R[2]
    XrInstruction add = CREATE_ABC(OP_ADD, 0, 1, 2);
    ASSERT_EQ_INT(GET_OPCODE(add), OP_ADD);
    
    // SUB R[3] = R[4] - R[5]
    XrInstruction sub = CREATE_ABC(OP_SUB, 3, 4, 5);
    ASSERT_EQ_INT(GET_OPCODE(sub), OP_SUB);
    
    // MUL R[6] = R[7] * R[8]
    XrInstruction mul = CREATE_ABC(OP_MUL, 6, 7, 8);
    ASSERT_EQ_INT(GET_OPCODE(mul), OP_MUL);
    
    // DIV R[9] = R[10] / R[11]
    XrInstruction div_i = CREATE_ABC(OP_DIV, 9, 10, 11);
    ASSERT_EQ_INT(GET_OPCODE(div_i), OP_DIV);
}

TEST(instruction_comparison) {
    // EQ comparison
    XrInstruction eq = CREATE_ABC(OP_EQ, 0, 1, 0);
    ASSERT_EQ_INT(GET_OPCODE(eq), OP_EQ);
    
    // LT comparison
    XrInstruction lt = CREATE_ABC(OP_LT, 2, 3, 0);
    ASSERT_EQ_INT(GET_OPCODE(lt), OP_LT);
}

TEST(instruction_jump) {
    // Forward jump +100
    XrInstruction jmp_fwd = CREATE_sJ(OP_JMP, 100);
    ASSERT_EQ_INT(GET_OPCODE(jmp_fwd), OP_JMP);
    ASSERT_EQ_INT(GETARG_sJ(jmp_fwd), 100);
    
    // Backward jump -50
    XrInstruction jmp_back = CREATE_sJ(OP_JMP, -50);
    ASSERT_EQ_INT(GET_OPCODE(jmp_back), OP_JMP);
    ASSERT_EQ_INT(GETARG_sJ(jmp_back), -50);
}

TEST(instruction_call) {
    // CALL R[A] = R[A](R[A+1]..R[A+B]), C=expected returns
    XrInstruction call = CREATE_ABC(OP_CALL, 0, 3, 1);
    
    ASSERT_EQ_INT(GET_OPCODE(call), OP_CALL);
    ASSERT_EQ_INT(GETARG_A(call), 0);  // function register
    ASSERT_EQ_INT(GETARG_B(call), 3);  // arg count
    ASSERT_EQ_INT(GETARG_C(call), 1);  // expected returns
}

TEST(instruction_return) {
    // RETURN R[A]..R[A+B-1]
    XrInstruction ret = CREATE_ABC(OP_RETURN, 0, 2, 0);
    
    ASSERT_EQ_INT(GET_OPCODE(ret), OP_RETURN);
    ASSERT_EQ_INT(GETARG_A(ret), 0);  // first return register
    ASSERT_EQ_INT(GETARG_B(ret), 2);  // return count
}

/* ========== Main ========== */

static void run_all_tests(void) {
    RUN_TEST_SUITE("Basic ABC Encoding");
    RUN_TEST(instruction_create_abc);
    RUN_TEST(instruction_create_abc_max_values);
    RUN_TEST(instruction_create_abc_zero);
    
    RUN_TEST_SUITE("ABx Format");
    RUN_TEST(instruction_create_abx);
    RUN_TEST(instruction_create_abx_max);
    
    RUN_TEST_SUITE("AsBx Format (Signed)");
    RUN_TEST(instruction_create_asbx_positive);
    RUN_TEST(instruction_create_asbx_negative);
    RUN_TEST(instruction_create_asbx_zero);
    RUN_TEST(instruction_create_asbx_max);
    RUN_TEST(instruction_create_asbx_min);
    
    RUN_TEST_SUITE("Ax Format");
    RUN_TEST(instruction_create_ax);
    RUN_TEST(instruction_create_ax_max);
    
    RUN_TEST_SUITE("sJ Format (Signed Jump)");
    RUN_TEST(instruction_create_sj_positive);
    RUN_TEST(instruction_create_sj_negative);
    RUN_TEST(instruction_create_sj_zero);
    
    RUN_TEST_SUITE("Argument Modification");
    RUN_TEST(instruction_setarg_a);
    RUN_TEST(instruction_setarg_b);
    RUN_TEST(instruction_setarg_c);
    RUN_TEST(instruction_setarg_bx);
    
    RUN_TEST_SUITE("Signed Arguments");
    RUN_TEST(instruction_signed_b);
    RUN_TEST(instruction_signed_b_positive);
    
    RUN_TEST_SUITE("Opcode Range");
    RUN_TEST(instruction_opcodes_fit_8bits);
    RUN_TEST(instruction_opcode_nop_is_last);
    
    RUN_TEST_SUITE("Specific Instructions");
    RUN_TEST(instruction_move);
    RUN_TEST(instruction_arithmetic);
    RUN_TEST(instruction_comparison);
    RUN_TEST(instruction_jump);
    RUN_TEST(instruction_call);
    RUN_TEST(instruction_return);
}

TEST_MAIN_BEGIN()
    printf("=== xray Instruction Encoding Unit Tests ===\n");
    run_all_tests();
TEST_MAIN_END()
