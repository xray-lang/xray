/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_peephole.c - Unit tests for ARM64 peephole optimiser patterns
 *
 * Each test constructs a small instruction buffer encoding a specific
 * pattern, runs xir_peephole(), and verifies the expected transformation.
 * Encodings are derived from the ARM Architecture Reference Manual.
 */

#include <stdio.h>
#include <assert.h>
#include <string.h>
#include "../../../src/jit/xir_peephole.h"

#define A64_NOP 0xD503201Fu

/* ========== Encoding helpers (local to tests) ========== */

/* STR Xt, [Xn, #imm12*8]  unsigned-offset 64-bit
 * 11_111_0_01_00_imm12_Rn_Rt */
static uint32_t enc_str_x(uint32_t rt, uint32_t rn, uint32_t imm12) {
    return 0xF9000000u | (imm12 << 10) | (rn << 5) | rt;
}

/* LDR Xt, [Xn, #imm12*8]  unsigned-offset 64-bit */
static uint32_t enc_ldr_x(uint32_t rt, uint32_t rn, uint32_t imm12) {
    return 0xF9400000u | (imm12 << 10) | (rn << 5) | rt;
}

/* STR Wt, [Xn, #imm12*4]  unsigned-offset 32-bit */
static uint32_t enc_str_w(uint32_t rt, uint32_t rn, uint32_t imm12) {
    return 0xB9000000u | (imm12 << 10) | (rn << 5) | rt;
}

/* LDR Wt, [Xn, #imm12*4]  unsigned-offset 32-bit */
static uint32_t enc_ldr_w(uint32_t rt, uint32_t rn, uint32_t imm12) {
    return 0xB9400000u | (imm12 << 10) | (rn << 5) | rt;
}

/* MOV Xd, Xn  = ORR Xd, XZR, Xn */
static uint32_t enc_mov_x(uint32_t rd, uint32_t rm) {
    return 0xAA0003E0u | (rm << 16) | rd;
}

/* CMP Xn, #imm12 = SUBS XZR, Xn, #imm12 */
static uint32_t enc_cmp_imm(uint32_t rn, uint32_t imm12) {
    return 0xF100001Fu | (imm12 << 10) | (rn << 5);
}

/* B.cond off19  (offset in instruction units, signed) */
static uint32_t enc_bcond(uint32_t cond, int32_t off19) {
    return 0x54000000u | (((uint32_t)off19 & 0x7FFFFu) << 5) | cond;
}

/* CBZ Xt, off19 */
static uint32_t enc_cbz(uint32_t rt, int32_t off19) {
    return 0xB4000000u | (((uint32_t)off19 & 0x7FFFFu) << 5) | rt;
}

/* CBNZ Xt, off19 */
static uint32_t enc_cbnz(uint32_t rt, int32_t off19) {
    return 0xB5000000u | (((uint32_t)off19 & 0x7FFFFu) << 5) | rt;
}

/* SUBS Xd, Xn, Xm */
static uint32_t enc_subs_reg(uint32_t rd, uint32_t rn, uint32_t rm) {
    return 0xEB000000u | (rm << 16) | (rn << 5) | rd;
}

/* CMP Xn, Xm = SUBS XZR, Xn, Xm */
static uint32_t enc_cmp_reg(uint32_t rn, uint32_t rm) {
    return enc_subs_reg(31, rn, rm);
}

/* MOVZ Xd, #imm16, LSL #hw*16 */
static uint32_t enc_movz_x(uint32_t rd, uint32_t imm16, uint32_t hw) {
    return 0xD2800000u | (hw << 21) | (imm16 << 5) | rd;
}

/* MOVK Xd, #imm16, LSL #hw*16 */
static uint32_t enc_movk_x(uint32_t rd, uint32_t imm16, uint32_t hw) {
    return 0xF2800000u | (hw << 21) | (imm16 << 5) | rd;
}

/* B off26  (unconditional branch, offset in instruction units, signed) */
static uint32_t enc_b(int32_t off26) {
    return 0x14000000u | ((uint32_t)off26 & 0x3FFFFFFu);
}

/* STP Xt1, Xt2, [Xn, #imm7*8]  offset 64-bit */
static uint32_t enc_stp_x(uint32_t rt1, uint32_t rt2, uint32_t rn, int32_t imm7) {
    return 0xA9000000u | (((uint32_t)imm7 & 0x7Fu) << 15) |
           (rt2 << 10) | (rn << 5) | rt1;
}

/* LDP Xt1, Xt2, [Xn, #imm7*8]  offset 64-bit */
static uint32_t enc_ldp_x(uint32_t rt1, uint32_t rt2, uint32_t rn, int32_t imm7) {
    return 0xA9400000u | (((uint32_t)imm7 & 0x7Fu) << 15) |
           (rt2 << 10) | (rn << 5) | rt1;
}

/* Condition codes */
#define CC_EQ 0u
#define CC_NE 1u

/* ========== Helper: build & run peephole ========== */

static uint32_t run_peephole(uint32_t *code, uint32_t count) {
    A64Buf buf;
    memset(&buf, 0, sizeof(buf));
    buf.code = code;
    buf.count = count;
    return xir_peephole(&buf);
}

/* ========== Pattern 1: STR+LDR → NOP the LDR (64-bit) ========== */

static void test_pattern1_str_ldr_64(void) {
    fprintf(stderr, "  test_pattern1_str_ldr_64...");

    /* STR X0, [X1, #8] ; LDR X0, [X1, #8] → NOP the LDR */
    uint32_t code[2] = {
        enc_str_x(0, 1, 1),  /* STR X0, [X1, #8] (imm12=1 means 1*8=8 bytes) */
        enc_ldr_x(0, 1, 1),  /* LDR X0, [X1, #8] */
    };
    uint32_t nops = run_peephole(code, 2);
    assert(nops == 1);
    assert(code[0] == enc_str_x(0, 1, 1));  /* STR unchanged */
    assert(code[1] == A64_NOP);              /* LDR replaced */

    fprintf(stderr, " PASS\n");
}

/* ========== Pattern 1b: STR+LDR → NOP the LDR (32-bit) ========== */

static void test_pattern1_str_ldr_32(void) {
    fprintf(stderr, "  test_pattern1_str_ldr_32...");

    /* STR W5, [X3, #16] ; LDR W5, [X3, #16] → NOP the LDR */
    uint32_t code[2] = {
        enc_str_w(5, 3, 4),  /* STR W5, [X3, #16] (imm12=4 means 4*4=16 bytes) */
        enc_ldr_w(5, 3, 4),  /* LDR W5, [X3, #16] */
    };
    uint32_t nops = run_peephole(code, 2);
    assert(nops == 1);
    assert(code[1] == A64_NOP);

    fprintf(stderr, " PASS\n");
}

/* ========== Pattern 1: no match when different offset ========== */

static void test_pattern1_no_match_diff_offset(void) {
    fprintf(stderr, "  test_pattern1_no_match_diff_offset...");

    uint32_t code[2] = {
        enc_str_x(0, 1, 1),  /* STR X0, [X1, #8] */
        enc_ldr_x(0, 1, 2),  /* LDR X0, [X1, #16]  — different offset */
    };
    uint32_t nops = run_peephole(code, 2);
    assert(nops == 0);

    fprintf(stderr, " PASS\n");
}

/* ========== Pattern 2: MOV Xd, Xd → NOP ========== */

static void test_pattern2_mov_self(void) {
    fprintf(stderr, "  test_pattern2_mov_self...");

    /* MOV X5,X5 followed by a harmless NOP (peephole requires count >= 2) */
    uint32_t code[2] = { enc_mov_x(5, 5), A64_NOP };
    uint32_t nops = run_peephole(code, 2);
    assert(nops == 1);
    assert(code[0] == A64_NOP);

    fprintf(stderr, " PASS\n");
}

/* ========== Pattern 2: MOV Xd, Xm (d!=m) → no match ========== */

static void test_pattern2_mov_diff(void) {
    fprintf(stderr, "  test_pattern2_mov_diff...");

    uint32_t orig = enc_mov_x(3, 7);
    uint32_t code[2] = { orig, A64_NOP };  /* MOV X3, X7 + NOP padding */
    uint32_t nops = run_peephole(code, 2);
    assert(nops == 0);
    assert(code[0] == orig);

    fprintf(stderr, " PASS\n");
}

/* ========== Pattern 3: CMP Xn,#0 + B.EQ → CBZ ========== */

static void test_pattern3_cmp_beq_cbz(void) {
    fprintf(stderr, "  test_pattern3_cmp_beq_cbz...");

    /* CMP X2, #0 ; B.EQ +10  → CBZ X2, +11 (offset adjusted by +1) */
    uint32_t code[2] = {
        enc_cmp_imm(2, 0),       /* CMP X2, #0 */
        enc_bcond(CC_EQ, 10),    /* B.EQ +10 */
    };
    uint32_t nops = run_peephole(code, 2);
    assert(nops == 1);
    assert(code[0] == enc_cbz(2, 11));  /* CBZ X2, +11 */
    assert(code[1] == A64_NOP);

    fprintf(stderr, " PASS\n");
}

/* ========== Pattern 3: CMP Xn,#0 + B.NE → CBNZ ========== */

static void test_pattern3_cmp_bne_cbnz(void) {
    fprintf(stderr, "  test_pattern3_cmp_bne_cbnz...");

    uint32_t code[2] = {
        enc_cmp_imm(7, 0),       /* CMP X7, #0 */
        enc_bcond(CC_NE, 5),     /* B.NE +5 */
    };
    uint32_t nops = run_peephole(code, 2);
    assert(nops == 1);
    assert(code[0] == enc_cbnz(7, 6));  /* CBNZ X7, +6 */
    assert(code[1] == A64_NOP);

    fprintf(stderr, " PASS\n");
}

/* ========== Pattern 3: CMP Xn,#1 → no match (imm != 0) ========== */

static void test_pattern3_no_match_nonzero(void) {
    fprintf(stderr, "  test_pattern3_no_match_nonzero...");

    uint32_t code[2] = {
        enc_cmp_imm(2, 1),       /* CMP X2, #1 */
        enc_bcond(CC_EQ, 10),    /* B.EQ +10 */
    };
    uint32_t nops = run_peephole(code, 2);
    assert(nops == 0);

    fprintf(stderr, " PASS\n");
}

/* ========== Pattern 4: STR+STR adjacent → STP ========== */

static void test_pattern4_str_str_stp(void) {
    fprintf(stderr, "  test_pattern4_str_str_stp...");

    /* STR X0, [X29, #0] ; STR X1, [X29, #8] → STP X0, X1, [X29, #0] */
    uint32_t code[2] = {
        enc_str_x(0, 29, 0),    /* STR X0, [X29, #0] (imm12=0) */
        enc_str_x(1, 29, 1),    /* STR X1, [X29, #8] (imm12=1) */
    };
    uint32_t nops = run_peephole(code, 2);
    assert(nops == 1);
    assert(code[0] == enc_stp_x(0, 1, 29, 0));  /* STP X0, X1, [X29, #0] */
    assert(code[1] == A64_NOP);

    fprintf(stderr, " PASS\n");
}

/* ========== Pattern 4: LDR+LDR adjacent → LDP ========== */

static void test_pattern4_ldr_ldr_ldp(void) {
    fprintf(stderr, "  test_pattern4_ldr_ldr_ldp...");

    /* LDR X2, [SP, #16] ; LDR X3, [SP, #24] → LDP X2, X3, [SP, #16] */
    uint32_t code[2] = {
        enc_ldr_x(2, 31, 2),    /* LDR X2, [SP, #16] (imm12=2 → 2*8=16) */
        enc_ldr_x(3, 31, 3),    /* LDR X3, [SP, #24] (imm12=3 → 3*8=24) */
    };
    uint32_t nops = run_peephole(code, 2);
    assert(nops == 1);
    assert(code[0] == enc_ldp_x(2, 3, 31, 2));  /* LDP X2, X3, [SP, #16] */
    assert(code[1] == A64_NOP);

    fprintf(stderr, " PASS\n");
}

/* ========== Pattern 4: non-consecutive → no match ========== */

static void test_pattern4_no_match_gap(void) {
    fprintf(stderr, "  test_pattern4_no_match_gap...");

    /* STR X0, [X29, #0] ; STR X1, [X29, #16] — gap of 16 bytes, not 8 */
    uint32_t code[2] = {
        enc_str_x(0, 29, 0),
        enc_str_x(1, 29, 2),  /* imm12=2 → offset 16, not 8 */
    };
    uint32_t nops = run_peephole(code, 2);
    assert(nops == 0);

    fprintf(stderr, " PASS\n");
}

/* ========== Pattern 5: SUBS + CMP same operands → NOP the CMP ========== */

static void test_pattern5_redundant_cmp(void) {
    fprintf(stderr, "  test_pattern5_redundant_cmp...");

    /* SUBS X0, X1, X2 ; CMP X1, X2 → NOP the CMP */
    uint32_t code[2] = {
        enc_subs_reg(0, 1, 2),   /* SUBS X0, X1, X2 */
        enc_cmp_reg(1, 2),       /* CMP X1, X2  (= SUBS XZR, X1, X2) */
    };
    uint32_t nops = run_peephole(code, 2);
    assert(nops == 1);
    assert(code[0] == enc_subs_reg(0, 1, 2));  /* SUBS unchanged */
    assert(code[1] == A64_NOP);

    fprintf(stderr, " PASS\n");
}

/* ========== Pattern 5: SUBS + CMP different operands → no match ========== */

static void test_pattern5_no_match_diff_ops(void) {
    fprintf(stderr, "  test_pattern5_no_match_diff_ops...");

    uint32_t code[2] = {
        enc_subs_reg(0, 1, 2),   /* SUBS X0, X1, X2 */
        enc_cmp_reg(3, 4),       /* CMP X3, X4 — different operands */
    };
    uint32_t nops = run_peephole(code, 2);
    assert(nops == 0);

    fprintf(stderr, " PASS\n");
}

/* ========== Pattern 6: MOVZ #0 + MOVK #imm → MOVZ #imm ========== */

static void test_pattern6_movz_movk(void) {
    fprintf(stderr, "  test_pattern6_movz_movk...");

    /* MOVZ X0, #0 ; MOVK X0, #42, LSL #0 → MOVZ X0, #42 */
    uint32_t code[2] = {
        enc_movz_x(0, 0, 0),     /* MOVZ X0, #0, LSL#0 */
        enc_movk_x(0, 42, 0),    /* MOVK X0, #42, LSL#0 */
    };
    uint32_t nops = run_peephole(code, 2);
    assert(nops == 1);
    assert(code[0] == enc_movz_x(0, 42, 0));  /* MOVZ X0, #42 */
    assert(code[1] == A64_NOP);

    fprintf(stderr, " PASS\n");
}

/* ========== Pattern 6: MOVZ #0 + MOVK LSL#16 → no match (different hw) ========== */

static void test_pattern6_no_match_hw(void) {
    fprintf(stderr, "  test_pattern6_no_match_hw...");

    uint32_t code[2] = {
        enc_movz_x(0, 0, 0),     /* MOVZ X0, #0, LSL#0 */
        enc_movk_x(0, 42, 1),    /* MOVK X0, #42, LSL#16 — hw=1 */
    };
    uint32_t nops = run_peephole(code, 2);
    assert(nops == 0);

    fprintf(stderr, " PASS\n");
}

/* ========== Pattern 7: B.cond +2 ; B target → inverted B.cond ========== */

static void test_pattern7_bcond_over_b(void) {
    fprintf(stderr, "  test_pattern7_bcond_over_b...");

    /* B.EQ +2 ; B +100 ; <something>
     * → B.NE +101 ; NOP ; <something>
     * (B at i+1 has offset +100 relative to i+1; new B.NE at i has offset +101) */
    uint32_t code[3] = {
        enc_bcond(CC_EQ, 2),     /* B.EQ +2 (skip the B instruction) */
        enc_b(100),              /* B +100 */
        A64_NOP,                 /* placeholder (needed: i+2 must exist) */
    };
    uint32_t nops = run_peephole(code, 3);
    assert(nops == 1);
    assert(code[0] == enc_bcond(CC_NE, 101));  /* B.NE +101 */
    assert(code[1] == A64_NOP);

    fprintf(stderr, " PASS\n");
}

/* ========== Pattern 7: B.cond +3 → no match (not +2) ========== */

static void test_pattern7_no_match_offset(void) {
    fprintf(stderr, "  test_pattern7_no_match_offset...");

    uint32_t code[3] = {
        enc_bcond(CC_EQ, 3),     /* B.EQ +3 (not +2) */
        enc_b(100),
        A64_NOP,
    };
    uint32_t nops = run_peephole(code, 3);
    assert(nops == 0);

    fprintf(stderr, " PASS\n");
}

/* ========== Pattern 8: duplicate STR → NOP the first ========== */

static void test_pattern8_dup_str(void) {
    fprintf(stderr, "  test_pattern8_dup_str...");

    /* STR X0, [X1, #8] ; STR X0, [X1, #8] → NOP first STR */
    uint32_t str = enc_str_x(0, 1, 1);
    uint32_t code[2] = { str, str };
    uint32_t nops = run_peephole(code, 2);
    assert(nops == 1);
    assert(code[0] == A64_NOP);       /* first STR replaced */
    assert(code[1] == str);           /* second STR kept */

    fprintf(stderr, " PASS\n");
}

/* ========== Pattern 8: duplicate 32-bit STR → NOP the first ========== */

static void test_pattern8_dup_str_32(void) {
    fprintf(stderr, "  test_pattern8_dup_str_32...");

    uint32_t str = enc_str_w(3, 5, 4);
    uint32_t code[2] = { str, str };
    uint32_t nops = run_peephole(code, 2);
    assert(nops == 1);
    assert(code[0] == A64_NOP);
    assert(code[1] == str);

    fprintf(stderr, " PASS\n");
}

/* ========== Pattern 8: different STR → no match ========== */

static void test_pattern8_no_match_diff(void) {
    fprintf(stderr, "  test_pattern8_no_match_diff...");

    /* STR X0,[X1,#8] ; STR X0,[X1,#24] — not consecutive, not duplicate */
    uint32_t code[2] = {
        enc_str_x(0, 1, 1),  /* offset #8 */
        enc_str_x(0, 1, 3),  /* offset #24 */
    };
    uint32_t nops = run_peephole(code, 2);
    assert(nops == 0);

    fprintf(stderr, " PASS\n");
}

/* ========== Combined: multiple patterns in one buffer ========== */

static void test_combined(void) {
    fprintf(stderr, "  test_combined...");

    /* Mix of patterns: MOV X0,X0; STR+LDR; CMP+B.EQ */
    uint32_t code[5] = {
        enc_mov_x(0, 0),          /* Pattern 2: MOV X0,X0 → NOP */
        enc_str_x(3, 4, 2),       /* Pattern 1: STR X3,[X4,#16] */
        enc_ldr_x(3, 4, 2),       /*            LDR X3,[X4,#16] → NOP */
        enc_cmp_imm(5, 0),        /* Pattern 3: CMP X5,#0 */
        enc_bcond(CC_NE, 20),     /*            B.NE +20 → CBNZ X5,+21 */
    };
    uint32_t nops = run_peephole(code, 5);
    assert(nops == 3);
    assert(code[0] == A64_NOP);             /* MOV self → NOP */
    assert(code[2] == A64_NOP);             /* LDR → NOP */
    assert(code[3] == enc_cbnz(5, 21));     /* CMP+B.NE → CBNZ */
    assert(code[4] == A64_NOP);             /* B.NE → NOP */

    fprintf(stderr, " PASS\n");
}

/* ========== Edge: empty buffer ========== */

static void test_empty(void) {
    fprintf(stderr, "  test_empty...");

    A64Buf buf;
    memset(&buf, 0, sizeof(buf));
    buf.code = NULL;
    buf.count = 0;
    uint32_t nops = xir_peephole(&buf);
    assert(nops == 0);

    fprintf(stderr, " PASS\n");
}

/* ========== Edge: single instruction (no pair patterns) ========== */

static void test_single_nop(void) {
    fprintf(stderr, "  test_single_nop...");

    uint32_t code[2] = { A64_NOP, A64_NOP };
    uint32_t nops = run_peephole(code, 2);
    assert(nops == 0);

    fprintf(stderr, " PASS\n");
}

/* ========== Driver ========== */

int main(void) {
    fprintf(stderr, "[test_peephole]\n");

    /* Pattern 1: STR+LDR elimination */
    test_pattern1_str_ldr_64();
    test_pattern1_str_ldr_32();
    test_pattern1_no_match_diff_offset();

    /* Pattern 2: MOV self elimination */
    test_pattern2_mov_self();
    test_pattern2_mov_diff();

    /* Pattern 3: CMP #0 + B.cond → CBZ/CBNZ fusion */
    test_pattern3_cmp_beq_cbz();
    test_pattern3_cmp_bne_cbnz();
    test_pattern3_no_match_nonzero();

    /* Pattern 4: LDP/STP fusion */
    test_pattern4_str_str_stp();
    test_pattern4_ldr_ldr_ldp();
    test_pattern4_no_match_gap();

    /* Pattern 5: Redundant CMP elimination */
    test_pattern5_redundant_cmp();
    test_pattern5_no_match_diff_ops();

    /* Pattern 6: MOVZ+MOVK simplification */
    test_pattern6_movz_movk();
    test_pattern6_no_match_hw();

    /* Pattern 7: B.cond over B → inverted B.cond */
    test_pattern7_bcond_over_b();
    test_pattern7_no_match_offset();

    /* Pattern 8: Duplicate STR elimination */
    test_pattern8_dup_str();
    test_pattern8_dup_str_32();
    test_pattern8_no_match_diff();

    /* Combined + edge cases */
    test_combined();
    test_empty();
    test_single_nop();

    fprintf(stderr, "[test_peephole] ALL PASSED (%d tests)\n", 22);
    return 0;
}
