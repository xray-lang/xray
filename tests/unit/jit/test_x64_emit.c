/*
 * test_x64_emit.c - Unit tests for x86-64 instruction encoding
 *
 * Each test encodes one instruction via the xir_x64.h API, then asserts
 * the emitted bytes match the reference encoding from the Intel SDM.
 *
 * Coverage: integer ALU, logical, compare, move, branch, stack, SSE2.
 * Edge cases: REX prefix for r8-r15, ModR/M rbp/rsp special handling.
 */

#ifdef __x86_64__

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../src/jit/xir_x64.h"

/* ========== Test Helpers ========== */

#define BUF_SIZE 64

static uint8_t g_mem[BUF_SIZE];
static X64Buf  g_buf;

static void reset(void) {
    memset(g_mem, 0xCC, BUF_SIZE);  /* fill with INT3 for safety */
    x64_buf_init(&g_buf, g_mem, BUF_SIZE);
}

/* Assert emitted bytes match expected */
static void check_bytes(const char *test_name, const uint8_t *expected,
                        uint32_t expected_len) {
    if (g_buf.pos != expected_len) {
        fprintf(stderr, "  FAIL %s: length %u != expected %u\n",
                test_name, g_buf.pos, expected_len);
        assert(g_buf.pos == expected_len);
    }
    if (memcmp(g_mem, expected, expected_len) != 0) {
        fprintf(stderr, "  FAIL %s: bytes mismatch\n    got:    ", test_name);
        for (uint32_t i = 0; i < g_buf.pos; i++)
            fprintf(stderr, "%02X ", g_mem[i]);
        fprintf(stderr, "\n    expect: ");
        for (uint32_t i = 0; i < expected_len; i++)
            fprintf(stderr, "%02X ", expected[i]);
        fprintf(stderr, "\n");
        assert(0);
    }
    fprintf(stderr, "  %s... PASS\n", test_name);
}

/* ========== Integer Arithmetic ========== */

/* ADD RAX, RCX:  REX.W(48) 01 C8 */
static void test_add_rr_low(void) {
    reset();
    x64_add_rr(&g_buf, X64_RAX, X64_RCX);
    uint8_t exp[] = { 0x48, 0x01, 0xC8 };
    check_bytes("add_rr_low", exp, sizeof(exp));
}

/* ADD R8, R9:  REX.WRB(4D) 01 C8 */
static void test_add_rr_high(void) {
    reset();
    x64_add_rr(&g_buf, X64_R8, X64_R9);
    /* R9(src)>7 → REX.R, R8(dst)>7 → REX.B */
    uint8_t exp[] = { 0x4D, 0x01, 0xC8 };
    check_bytes("add_rr_high", exp, sizeof(exp));
}

/* ADD RBX, 42:  REX.W(48) 83 C3 2A  (imm8 form) */
static void test_add_ri_imm8(void) {
    reset();
    x64_add_ri(&g_buf, X64_RBX, 42);
    uint8_t exp[] = { 0x48, 0x83, 0xC3, 0x2A };
    check_bytes("add_ri_imm8", exp, sizeof(exp));
}

/* ADD RBX, 0x1000:  REX.W(48) 81 C3 00 10 00 00  (imm32 form) */
static void test_add_ri_imm32(void) {
    reset();
    x64_add_ri(&g_buf, X64_RBX, 0x1000);
    uint8_t exp[] = { 0x48, 0x81, 0xC3, 0x00, 0x10, 0x00, 0x00 };
    check_bytes("add_ri_imm32", exp, sizeof(exp));
}

/* SUB RDX, RSI:  REX.W(48) 29 F2 */
static void test_sub_rr(void) {
    reset();
    x64_sub_rr(&g_buf, X64_RDX, X64_RSI);
    uint8_t exp[] = { 0x48, 0x29, 0xF2 };
    check_bytes("sub_rr", exp, sizeof(exp));
}

/* SUB R12, 10:  REX.WB(49) 83 EC 0A */
static void test_sub_ri_high(void) {
    reset();
    x64_sub_ri(&g_buf, X64_R12, 10);
    uint8_t exp[] = { 0x49, 0x83, 0xEC, 0x0A };
    check_bytes("sub_ri_high", exp, sizeof(exp));
}

/* IMUL RDI, RSI:  REX.W(48) 0F AF FE */
static void test_imul_rr(void) {
    reset();
    x64_imul_rr(&g_buf, X64_RDI, X64_RSI);
    uint8_t exp[] = { 0x48, 0x0F, 0xAF, 0xFE };
    check_bytes("imul_rr", exp, sizeof(exp));
}

/* NEG RCX:  REX.W(48) F7 D9 */
static void test_neg(void) {
    reset();
    x64_neg_r(&g_buf, X64_RCX);
    uint8_t exp[] = { 0x48, 0xF7, 0xD9 };
    check_bytes("neg", exp, sizeof(exp));
}

/* CQO:  REX.W(48) 99 */
static void test_cqo(void) {
    reset();
    x64_cqo(&g_buf);
    uint8_t exp[] = { 0x48, 0x99 };
    check_bytes("cqo", exp, sizeof(exp));
}

/* IDIV RBX:  REX.W(48) F7 FB */
static void test_idiv(void) {
    reset();
    x64_idiv_r(&g_buf, X64_RBX);
    uint8_t exp[] = { 0x48, 0xF7, 0xFB };
    check_bytes("idiv", exp, sizeof(exp));
}

/* ========== Logical ========== */

/* AND RCX, RDX:  REX.W(48) 21 D1 */
static void test_and_rr(void) {
    reset();
    x64_and_rr(&g_buf, X64_RCX, X64_RDX);
    uint8_t exp[] = { 0x48, 0x21, 0xD1 };
    check_bytes("and_rr", exp, sizeof(exp));
}

/* OR RAX, RDI:  REX.W(48) 09 F8 */
static void test_or_rr(void) {
    reset();
    x64_or_rr(&g_buf, X64_RAX, X64_RDI);
    uint8_t exp[] = { 0x48, 0x09, 0xF8 };
    check_bytes("or_rr", exp, sizeof(exp));
}

/* XOR RDX, RDX:  REX.W(48) 31 D2 */
static void test_xor_rr(void) {
    reset();
    x64_xor_rr(&g_buf, X64_RDX, X64_RDX);
    uint8_t exp[] = { 0x48, 0x31, 0xD2 };
    check_bytes("xor_rr", exp, sizeof(exp));
}

/* NOT RSI:  REX.W(48) F7 D6 */
static void test_not(void) {
    reset();
    x64_not_r(&g_buf, X64_RSI);
    uint8_t exp[] = { 0x48, 0xF7, 0xD6 };
    check_bytes("not", exp, sizeof(exp));
}

/* SHL RCX, CL:  REX.W(48) D3 E1 */
static void test_shl(void) {
    reset();
    x64_shl_rcl(&g_buf, X64_RCX);
    uint8_t exp[] = { 0x48, 0xD3, 0xE1 };
    check_bytes("shl", exp, sizeof(exp));
}

/* SHR RAX, CL:  REX.W(48) D3 E8 */
static void test_shr(void) {
    reset();
    x64_shr_rcl(&g_buf, X64_RAX);
    uint8_t exp[] = { 0x48, 0xD3, 0xE8 };
    check_bytes("shr", exp, sizeof(exp));
}

/* SAR RDX, CL:  REX.W(48) D3 FA */
static void test_sar(void) {
    reset();
    x64_sar_rcl(&g_buf, X64_RDX);
    uint8_t exp[] = { 0x48, 0xD3, 0xFA };
    check_bytes("sar", exp, sizeof(exp));
}

/* ========== Compare / Test ========== */

/* CMP RAX, RCX:  REX.W(48) 39 C8 */
static void test_cmp_rr(void) {
    reset();
    x64_cmp_rr(&g_buf, X64_RAX, X64_RCX);
    uint8_t exp[] = { 0x48, 0x39, 0xC8 };
    check_bytes("cmp_rr", exp, sizeof(exp));
}

/* CMP RDI, 0:  REX.W(48) 83 FF 00 (imm8) */
static void test_cmp_ri_zero(void) {
    reset();
    x64_cmp_ri(&g_buf, X64_RDI, 0);
    uint8_t exp[] = { 0x48, 0x83, 0xFF, 0x00 };
    check_bytes("cmp_ri_zero", exp, sizeof(exp));
}

/* TEST RCX, RCX:  REX.W(48) 85 C9 */
static void test_test_rr(void) {
    reset();
    x64_test_rr(&g_buf, X64_RCX, X64_RCX);
    uint8_t exp[] = { 0x48, 0x85, 0xC9 };
    check_bytes("test_rr", exp, sizeof(exp));
}

/* ========== Move ========== */

/* MOV RDX, RCX:  REX.W(48) 89 CA */
static void test_mov_rr(void) {
    reset();
    x64_mov_rr(&g_buf, X64_RDX, X64_RCX);
    uint8_t exp[] = { 0x48, 0x89, 0xCA };
    check_bytes("mov_rr", exp, sizeof(exp));
}

/* MOV R10, R13:  REX.WRB(4D) 89 EA */
static void test_mov_rr_high(void) {
    reset();
    x64_mov_rr(&g_buf, X64_R10, X64_R13);
    uint8_t exp[] = { 0x4D, 0x89, 0xEA };
    check_bytes("mov_rr_high", exp, sizeof(exp));
}

/* MOV RAX, 0x123456789ABCDEF0:  REX.W(48) B8 F0 DE BC 9A 78 56 34 12 */
static void test_mov_ri64(void) {
    reset();
    x64_mov_ri64(&g_buf, X64_RAX, 0x123456789ABCDEF0ULL);
    uint8_t exp[] = { 0x48, 0xB8, 0xF0, 0xDE, 0xBC, 0x9A, 0x78, 0x56, 0x34, 0x12 };
    check_bytes("mov_ri64", exp, sizeof(exp));
}

/* MOV R8, imm64:  REX.WB(49) B8 ... */
static void test_mov_ri64_high(void) {
    reset();
    x64_mov_ri64(&g_buf, X64_R8, 42);
    uint8_t exp[] = { 0x49, 0xB8, 42, 0, 0, 0, 0, 0, 0, 0 };
    check_bytes("mov_ri64_high", exp, sizeof(exp));
}

/* MOV EAX, 0x42:  B8 42 00 00 00  (32-bit, no REX.W → zero-extends) */
static void test_mov_ri32(void) {
    reset();
    x64_mov_ri32(&g_buf, X64_RAX, 0x42);
    uint8_t exp[] = { 0xB8, 0x42, 0x00, 0x00, 0x00 };
    check_bytes("mov_ri32", exp, sizeof(exp));
}

/* MOV R9D, 0xFF:  REX.B(41) B9 FF 00 00 00 */
static void test_mov_ri32_high(void) {
    reset();
    x64_mov_ri32(&g_buf, X64_R9, 0xFF);
    uint8_t exp[] = { 0x41, 0xB9, 0xFF, 0x00, 0x00, 0x00 };
    check_bytes("mov_ri32_high", exp, sizeof(exp));
}

/* ========== Memory Load/Store ========== */

/* MOV RAX, [RCX]:  REX.W(48) 8B 01 */
static void test_mov_rm_no_disp(void) {
    reset();
    x64_mov_rm(&g_buf, X64_RAX, X64_RCX, 0);
    uint8_t exp[] = { 0x48, 0x8B, 0x01 };
    check_bytes("mov_rm_no_disp", exp, sizeof(exp));
}

/* MOV RAX, [RCX+8]:  REX.W(48) 8B 41 08 */
static void test_mov_rm_disp8(void) {
    reset();
    x64_mov_rm(&g_buf, X64_RAX, X64_RCX, 8);
    uint8_t exp[] = { 0x48, 0x8B, 0x41, 0x08 };
    check_bytes("mov_rm_disp8", exp, sizeof(exp));
}

/* MOV RAX, [RCX+0x200]:  REX.W(48) 8B 81 00 02 00 00 */
static void test_mov_rm_disp32(void) {
    reset();
    x64_mov_rm(&g_buf, X64_RAX, X64_RCX, 0x200);
    uint8_t exp[] = { 0x48, 0x8B, 0x81, 0x00, 0x02, 0x00, 0x00 };
    check_bytes("mov_rm_disp32", exp, sizeof(exp));
}

/* Edge case: MOV RAX, [RBP]:  must encode as [RBP+0] since mod=00/rm=101 is RIP-relative
 * REX.W(48) 8B 45 00 */
static void test_mov_rm_rbp(void) {
    reset();
    x64_mov_rm(&g_buf, X64_RAX, X64_RBP, 0);
    /* rbp with disp=0 must use mod=01 with disp8=0 */
    uint8_t exp[] = { 0x48, 0x8B, 0x45, 0x00 };
    check_bytes("mov_rm_rbp", exp, sizeof(exp));
}

/* Edge case: MOV RAX, [RSP]:  needs SIB byte (rm=100 → SIB follows)
 * REX.W(48) 8B 04 24 */
static void test_mov_rm_rsp(void) {
    reset();
    x64_mov_rm(&g_buf, X64_RAX, X64_RSP, 0);
    uint8_t exp[] = { 0x48, 0x8B, 0x04, 0x24 };
    check_bytes("mov_rm_rsp", exp, sizeof(exp));
}

/* MOV RAX, [RSP+16]:  REX.W(48) 8B 44 24 10 */
static void test_mov_rm_rsp_disp8(void) {
    reset();
    x64_mov_rm(&g_buf, X64_RAX, X64_RSP, 16);
    uint8_t exp[] = { 0x48, 0x8B, 0x44, 0x24, 0x10 };
    check_bytes("mov_rm_rsp_disp8", exp, sizeof(exp));
}

/* MOV [RDI+16], RSI:  REX.W(48) 89 77 10 */
static void test_mov_mr(void) {
    reset();
    x64_mov_mr(&g_buf, X64_RDI, 16, X64_RSI);
    uint8_t exp[] = { 0x48, 0x89, 0x77, 0x10 };
    check_bytes("mov_mr", exp, sizeof(exp));
}

/* LEA RAX, [RCX+32]:  REX.W(48) 8D 41 20 */
static void test_lea(void) {
    reset();
    x64_lea(&g_buf, X64_RAX, X64_RCX, 32);
    uint8_t exp[] = { 0x48, 0x8D, 0x41, 0x20 };
    check_bytes("lea", exp, sizeof(exp));
}

/* ========== Branch ========== */

/* JMP rel32(0):  E9 00 00 00 00 */
static void test_jmp_rel32(void) {
    reset();
    x64_jmp_rel32(&g_buf, 0);
    uint8_t exp[] = { 0xE9, 0x00, 0x00, 0x00, 0x00 };
    check_bytes("jmp_rel32", exp, sizeof(exp));
}

/* JMP rel8(0x10):  EB 10 */
static void test_jmp_rel8(void) {
    reset();
    x64_jmp_rel8(&g_buf, 0x10);
    uint8_t exp[] = { 0xEB, 0x10 };
    check_bytes("jmp_rel8", exp, sizeof(exp));
}

/* JE rel32(0):  0F 84 00 00 00 00 */
static void test_je_rel32(void) {
    reset();
    x64_jcc_rel32(&g_buf, X64_CC_E, 0);
    uint8_t exp[] = { 0x0F, 0x84, 0x00, 0x00, 0x00, 0x00 };
    check_bytes("je_rel32", exp, sizeof(exp));
}

/* JG rel32(0):  0F 8F 00 00 00 00 */
static void test_jg_rel32(void) {
    reset();
    x64_jcc_rel32(&g_buf, X64_CC_G, 0);
    uint8_t exp[] = { 0x0F, 0x8F, 0x00, 0x00, 0x00, 0x00 };
    check_bytes("jg_rel32", exp, sizeof(exp));
}

/* CALL rel32(0):  E8 00 00 00 00 */
static void test_call_rel32(void) {
    reset();
    x64_call_rel32(&g_buf, 0);
    uint8_t exp[] = { 0xE8, 0x00, 0x00, 0x00, 0x00 };
    check_bytes("call_rel32", exp, sizeof(exp));
}

/* CALL RAX:  FF D0 */
static void test_call_r_low(void) {
    reset();
    x64_call_r(&g_buf, X64_RAX);
    uint8_t exp[] = { 0xFF, 0xD0 };
    check_bytes("call_r_low", exp, sizeof(exp));
}

/* CALL R12:  REX.B(41) FF D4 */
static void test_call_r_high(void) {
    reset();
    x64_call_r(&g_buf, X64_R12);
    uint8_t exp[] = { 0x41, 0xFF, 0xD4 };
    check_bytes("call_r_high", exp, sizeof(exp));
}

/* RET:  C3 */
static void test_ret(void) {
    reset();
    x64_ret(&g_buf);
    uint8_t exp[] = { 0xC3 };
    check_bytes("ret", exp, sizeof(exp));
}

/* NOP:  90 */
static void test_nop(void) {
    reset();
    x64_nop(&g_buf);
    uint8_t exp[] = { 0x90 };
    check_bytes("nop", exp, sizeof(exp));
}

/* ========== Stack ========== */

/* PUSH RBX:  53 */
static void test_push_low(void) {
    reset();
    x64_push_r(&g_buf, X64_RBX);
    uint8_t exp[] = { 0x53 };
    check_bytes("push_low", exp, sizeof(exp));
}

/* PUSH R15:  REX.B(41) 57 */
static void test_push_high(void) {
    reset();
    x64_push_r(&g_buf, X64_R15);
    uint8_t exp[] = { 0x41, 0x57 };
    check_bytes("push_high", exp, sizeof(exp));
}

/* POP RBX:  5B */
static void test_pop_low(void) {
    reset();
    x64_pop_r(&g_buf, X64_RBX);
    uint8_t exp[] = { 0x5B };
    check_bytes("pop_low", exp, sizeof(exp));
}

/* POP R14:  REX.B(41) 5E */
static void test_pop_high(void) {
    reset();
    x64_pop_r(&g_buf, X64_R14);
    uint8_t exp[] = { 0x41, 0x5E };
    check_bytes("pop_high", exp, sizeof(exp));
}

/* ========== Patch Helper ========== */

static void test_patch_rel32(void) {
    fprintf(stderr, "  patch_rel32...");
    reset();
    /* Emit JMP rel32 at offset 0 (5 bytes: E9 + 4-byte displacement) */
    x64_jmp_rel32(&g_buf, 0);
    /* Patch: target is at byte offset 20, rel32 field is at offset 1.
     * Displacement = target - (patch_pos + 4) = 20 - (1+4) = 15 */
    x64_patch_rel32(&g_buf, 1, 20);
    int32_t patched;
    memcpy(&patched, &g_mem[1], 4);
    assert(patched == 15);
    fprintf(stderr, " PASS\n");
}

/* ========== SETcc ========== */

/* SETE AL:  0F 94 C0 (no REX for RAX/AL — registers 0-3 don't need it) */
static void test_setcc_low(void) {
    reset();
    x64_setcc(&g_buf, X64_CC_E, X64_RAX);
    uint8_t exp[] = { 0x0F, 0x94, 0xC0 };
    check_bytes("setcc_e_al", exp, sizeof(exp));
}

/* SETE SIL:  REX(40) 0F 94 C6 (REX needed for RSI→SIL, avoid DH) */
static void test_setcc_rsi(void) {
    reset();
    x64_setcc(&g_buf, X64_CC_E, X64_RSI);
    uint8_t exp[] = { 0x40, 0x0F, 0x94, 0xC6 };
    check_bytes("setcc_e_sil", exp, sizeof(exp));
}

/* SETE R9B:  REX.B(41) 0F 94 C1 (REX.B for r9) */
static void test_setcc_high(void) {
    reset();
    x64_setcc(&g_buf, X64_CC_E, X64_R9);
    uint8_t exp[] = { 0x41, 0x0F, 0x94, 0xC1 };
    check_bytes("setcc_e_r9b", exp, sizeof(exp));
}

/* ========== CMOVcc ========== */

/* CMOVNE RAX, RCX:  REX.W(48) 0F 45 C1 */
static void test_cmov(void) {
    reset();
    x64_cmov_rr(&g_buf, X64_CC_NE, X64_RAX, X64_RCX);
    uint8_t exp[] = { 0x48, 0x0F, 0x45, 0xC1 };
    check_bytes("cmovne", exp, sizeof(exp));
}

/* ========== SSE2 ========== */

/* ADDSD XMM0, XMM1:  F2 0F 58 C1 */
static void test_addsd(void) {
    reset();
    x64_addsd(&g_buf, X64_XMM0, X64_XMM1);
    uint8_t exp[] = { 0xF2, 0x0F, 0x58, 0xC1 };
    check_bytes("addsd", exp, sizeof(exp));
}

/* SUBSD XMM2, XMM3:  F2 0F 5C D3 */
static void test_subsd(void) {
    reset();
    x64_subsd(&g_buf, X64_XMM2, X64_XMM3);
    uint8_t exp[] = { 0xF2, 0x0F, 0x5C, 0xD3 };
    check_bytes("subsd", exp, sizeof(exp));
}

/* MULSD XMM0, XMM0:  F2 0F 59 C0 */
static void test_mulsd(void) {
    reset();
    x64_mulsd(&g_buf, X64_XMM0, X64_XMM0);
    uint8_t exp[] = { 0xF2, 0x0F, 0x59, 0xC0 };
    check_bytes("mulsd", exp, sizeof(exp));
}

/* DIVSD XMM1, XMM0:  F2 0F 5E C8 */
static void test_divsd(void) {
    reset();
    x64_divsd(&g_buf, X64_XMM1, X64_XMM0);
    uint8_t exp[] = { 0xF2, 0x0F, 0x5E, 0xC8 };
    check_bytes("divsd", exp, sizeof(exp));
}

/* MOVSD XMM1, XMM2:  F2 0F 10 CA */
static void test_movsd_rr(void) {
    reset();
    x64_movsd_rr(&g_buf, X64_XMM1, X64_XMM2);
    uint8_t exp[] = { 0xF2, 0x0F, 0x10, 0xCA };
    check_bytes("movsd_rr", exp, sizeof(exp));
}

/* UCOMISD XMM0, XMM1:  66 0F 2E C1 */
static void test_ucomisd(void) {
    reset();
    x64_ucomisd(&g_buf, X64_XMM0, X64_XMM1);
    uint8_t exp[] = { 0x66, 0x0F, 0x2E, 0xC1 };
    check_bytes("ucomisd", exp, sizeof(exp));
}

/* CVTSI2SD XMM0, RAX:  F2 REX.W(48) 0F 2A C0 */
static void test_cvtsi2sd(void) {
    reset();
    x64_cvtsi2sd(&g_buf, X64_XMM0, X64_RAX);
    uint8_t exp[] = { 0xF2, 0x48, 0x0F, 0x2A, 0xC0 };
    check_bytes("cvtsi2sd", exp, sizeof(exp));
}

/* CVTTSD2SI RAX, XMM0:  F2 REX.W(48) 0F 2C C0 */
static void test_cvttsd2si(void) {
    reset();
    x64_cvttsd2si(&g_buf, X64_RAX, X64_XMM0);
    uint8_t exp[] = { 0xF2, 0x48, 0x0F, 0x2C, 0xC0 };
    check_bytes("cvttsd2si", exp, sizeof(exp));
}

/* ========== XORPD / MOVQ ========== */

/* XORPD XMM0, XMM0:  66 0F 57 C0 */
static void test_xorpd(void) {
    reset();
    x64_xorpd(&g_buf, X64_XMM0, X64_XMM0);
    uint8_t exp[] = { 0x66, 0x0F, 0x57, 0xC0 };
    check_bytes("xorpd", exp, sizeof(exp));
}

/* MOVQ XMM0, RAX:  66 REX.W(48) 0F 6E C0 */
static void test_movq_xmm_gp(void) {
    reset();
    x64_movq_xmm_gp(&g_buf, X64_XMM0, X64_RAX);
    uint8_t exp[] = { 0x66, 0x48, 0x0F, 0x6E, 0xC0 };
    check_bytes("movq_xmm_gp", exp, sizeof(exp));
}

/* MOVQ RAX, XMM0:  66 REX.W(48) 0F 7E C0 */
static void test_movq_gp_xmm(void) {
    reset();
    x64_movq_gp_xmm(&g_buf, X64_RAX, X64_XMM0);
    uint8_t exp[] = { 0x66, 0x48, 0x0F, 0x7E, 0xC0 };
    check_bytes("movq_gp_xmm", exp, sizeof(exp));
}

/* MOVSD XMM1, [RBP-8]:  F2 0F 10 4D F8 */
static void test_movsd_load(void) {
    reset();
    x64_movsd_rm(&g_buf, X64_XMM1, X64_RBP, -8);
    uint8_t exp[] = { 0xF2, 0x0F, 0x10, 0x4D, 0xF8 };
    check_bytes("movsd_load", exp, sizeof(exp));
}

/* MOVSD [RBP-8], XMM1:  F2 0F 11 4D F8 */
static void test_movsd_store(void) {
    reset();
    x64_movsd_mr(&g_buf, X64_RBP, -8, X64_XMM1);
    uint8_t exp[] = { 0xF2, 0x0F, 0x11, 0x4D, 0xF8 };
    check_bytes("movsd_store", exp, sizeof(exp));
}

/* ========== Sub-word memory ========== */

/* MOVZX r64, byte [RAX+0]:  REX.W 0F B6 00 */
static void test_movzx_rm8(void) {
    reset();
    x64_movzx_rm8(&g_buf, X64_RAX, X64_RAX, 0);
    uint8_t exp[] = { 0x48, 0x0F, 0xB6, 0x00 };
    check_bytes("movzx_rm8", exp, sizeof(exp));
}

/* MOVSX r64, byte [RAX+0]:  REX.W 0F BE 00 */
static void test_movsx_rm8(void) {
    reset();
    x64_movsx_rm8(&g_buf, X64_RAX, X64_RAX, 0);
    uint8_t exp[] = { 0x48, 0x0F, 0xBE, 0x00 };
    check_bytes("movsx_rm8", exp, sizeof(exp));
}

/* MOV [RAX+0], r8(CL):  40 88 08  (REX needed for CL in 64-bit mode regs 4-7) */
static void test_mov_mr8(void) {
    reset();
    x64_mov_mr8(&g_buf, X64_RAX, 0, X64_RCX);
    uint8_t exp[] = { 0x88, 0x08 };
    check_bytes("mov_mr8", exp, sizeof(exp));
}

/* MOV r32, [RBP-8]:  8B 45 F8  (32-bit load, zero-extend) */
static void test_mov_rm32(void) {
    reset();
    x64_mov_rm32(&g_buf, X64_RAX, X64_RBP, -8);
    uint8_t exp[] = { 0x8B, 0x45, 0xF8 };
    check_bytes("mov_rm32", exp, sizeof(exp));
}

/* MOV [RBP-8], r32:  89 45 F8  (32-bit store) */
static void test_mov_mr32(void) {
    reset();
    x64_mov_mr32(&g_buf, X64_RBP, -8, X64_RAX);
    uint8_t exp[] = { 0x89, 0x45, 0xF8 };
    check_bytes("mov_mr32", exp, sizeof(exp));
}

/* MOVSXD r64, [RBP-4]:  REX.W 63 45 FC */
static void test_movsxd(void) {
    reset();
    x64_movsxd_rm(&g_buf, X64_RAX, X64_RBP, -4);
    uint8_t exp[] = { 0x48, 0x63, 0x45, 0xFC };
    check_bytes("movsxd", exp, sizeof(exp));
}

/* ========== Shift/OR immediate ========== */

/* SHL RAX, 16:  REX.W C1 E0 10 */
static void test_shl_ri(void) {
    reset();
    x64_shl_ri(&g_buf, X64_RAX, 16);
    uint8_t exp[] = { 0x48, 0xC1, 0xE0, 0x10 };
    check_bytes("shl_ri", exp, sizeof(exp));
}

/* OR RAX, 5:  REX.W 83 C8 05 */
static void test_or_ri_imm8(void) {
    reset();
    x64_or_ri(&g_buf, X64_RAX, 5);
    uint8_t exp[] = { 0x48, 0x83, 0xC8, 0x05 };
    check_bytes("or_ri_imm8", exp, sizeof(exp));
}

/* ========== Driver ========== */

int main(void) {
    fprintf(stderr, "[test_x64_emit]\n");

    /* Integer arithmetic */
    test_add_rr_low();
    test_add_rr_high();
    test_add_ri_imm8();
    test_add_ri_imm32();
    test_sub_rr();
    test_sub_ri_high();
    test_imul_rr();
    test_neg();
    test_cqo();
    test_idiv();

    /* Logical */
    test_and_rr();
    test_or_rr();
    test_xor_rr();
    test_not();
    test_shl();
    test_shr();
    test_sar();

    /* Compare / Test */
    test_cmp_rr();
    test_cmp_ri_zero();
    test_test_rr();

    /* Move */
    test_mov_rr();
    test_mov_rr_high();
    test_mov_ri64();
    test_mov_ri64_high();
    test_mov_ri32();
    test_mov_ri32_high();

    /* Memory */
    test_mov_rm_no_disp();
    test_mov_rm_disp8();
    test_mov_rm_disp32();
    test_mov_rm_rbp();
    test_mov_rm_rsp();
    test_mov_rm_rsp_disp8();
    test_mov_mr();
    test_lea();

    /* Branch */
    test_jmp_rel32();
    test_jmp_rel8();
    test_je_rel32();
    test_jg_rel32();
    test_call_rel32();
    test_call_r_low();
    test_call_r_high();
    test_ret();
    test_nop();

    /* Stack */
    test_push_low();
    test_push_high();
    test_pop_low();
    test_pop_high();

    /* Patch */
    test_patch_rel32();

    /* SETcc / CMOVcc */
    test_setcc_low();
    test_setcc_rsi();
    test_setcc_high();
    test_cmov();

    /* SSE2 */
    test_addsd();
    test_subsd();
    test_mulsd();
    test_divsd();
    test_movsd_rr();
    test_ucomisd();
    test_cvtsi2sd();
    test_cvttsd2si();

    /* XORPD / MOVQ / MOVSD mem */
    test_xorpd();
    test_movq_xmm_gp();
    test_movq_gp_xmm();
    test_movsd_load();
    test_movsd_store();

    /* Sub-word memory */
    test_movzx_rm8();
    test_movsx_rm8();
    test_mov_mr8();
    test_mov_rm32();
    test_mov_mr32();
    test_movsxd();

    /* Shift/OR immediate */
    test_shl_ri();
    test_or_ri_imm8();

    int total = 65;
    fprintf(stderr, "[test_x64_emit] ALL PASSED (%d tests)\n", total);
    return 0;
}

#else // !__x86_64__

#include <stdio.h>
int main(void) {
    fprintf(stderr, "[test_x64_emit] SKIPPED (not x86-64)\n");
    return 0;
}

#endif // __x86_64__
