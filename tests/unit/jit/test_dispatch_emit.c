#include "xm_dispatch_emit_gen.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int test_riscv64_gp_rrr(void) {
    uint32_t code[11] = {0};
    Rv64Buf buf;
    rv64_buf_init(&buf, code, 11);

    if (!xm_dispatch_emit_riscv64_gp_rrr(XM_ADD, &buf, RV64_A0, RV64_A1, RV64_A2))
        return 1;
    if (!xm_dispatch_emit_riscv64_gp_rrr(XM_SUB, &buf, RV64_A3, RV64_A4, RV64_A5))
        return 1;
    if (!xm_dispatch_emit_riscv64_gp_rrr(XM_MUL, &buf, RV64_A6, RV64_A7, RV64_T0))
        return 1;
    if (!xm_dispatch_emit_riscv64_gp_rrr(XM_AND, &buf, RV64_A0, RV64_A1, RV64_A2))
        return 1;
    if (!xm_dispatch_emit_riscv64_gp_rrr(XM_OR, &buf, RV64_A3, RV64_A4, RV64_A5))
        return 1;
    if (!xm_dispatch_emit_riscv64_gp_rrr(XM_XOR, &buf, RV64_A6, RV64_A7, RV64_T0))
        return 1;
    if (!xm_dispatch_emit_riscv64_gp_rrr(XM_SHL, &buf, RV64_A0, RV64_A1, RV64_A2))
        return 1;
    if (!xm_dispatch_emit_riscv64_gp_rrr(XM_SHR, &buf, RV64_A3, RV64_A4, RV64_A5))
        return 1;
    /* NEG via driver passing rs1=X0; LT direct; GT same emit (caller swaps). */
    if (!xm_dispatch_emit_riscv64_gp_rrr(XM_NEG, &buf, RV64_A6, RV64_X0, RV64_A7))
        return 1;
    if (!xm_dispatch_emit_riscv64_gp_rrr(XM_LT, &buf, RV64_A0, RV64_A1, RV64_A2))
        return 1;
    if (!xm_dispatch_emit_riscv64_gp_rrr(XM_GT, &buf, RV64_A3, RV64_A5, RV64_A4))
        return 1;
    if (buf.count != 11)
        return 1;
    if (code[0] != rv64_add(RV64_A0, RV64_A1, RV64_A2))
        return 1;
    if (code[1] != rv64_sub(RV64_A3, RV64_A4, RV64_A5))
        return 1;
    if (code[2] != rv64_mul(RV64_A6, RV64_A7, RV64_T0))
        return 1;
    if (code[3] != rv64_and(RV64_A0, RV64_A1, RV64_A2))
        return 1;
    if (code[4] != rv64_or(RV64_A3, RV64_A4, RV64_A5))
        return 1;
    if (code[5] != rv64_xor(RV64_A6, RV64_A7, RV64_T0))
        return 1;
    if (code[6] != rv64_sll(RV64_A0, RV64_A1, RV64_A2))
        return 1;
    if (code[7] != rv64_sra(RV64_A3, RV64_A4, RV64_A5))
        return 1;
    if (code[8] != rv64_sub(RV64_A6, RV64_X0, RV64_A7))
        return 1;
    if (code[9] != rv64_slt(RV64_A0, RV64_A1, RV64_A2))
        return 1;
    if (code[10] != rv64_slt(RV64_A3, RV64_A5, RV64_A4))
        return 1;
    if (xm_dispatch_emit_riscv64_gp_rrr(XM_DIV, &buf, RV64_A0, RV64_A1, RV64_A2))
        return 1;
    if (buf.count != 11)
        return 1;
    return 0;
}

#if defined(__x86_64__) || defined(_M_X64)
static int test_x64_gp_rr(void) {
    uint8_t code[64];
    X64Buf buf;
    memset(code, 0, sizeof(code));
    x64_buf_init(&buf, code, (uint32_t) sizeof(code));

    if (!xm_dispatch_emit_x64_gp_rr_comm(XM_ADD, &buf, X64_RAX, X64_RBX))
        return 1;
    if (!xm_dispatch_emit_x64_gp_rr(XM_SUB, &buf, X64_RCX, X64_RDX))
        return 1;
    if (!xm_dispatch_emit_x64_gp_rr_comm(XM_MUL, &buf, X64_RSI, X64_RDI))
        return 1;
    if (!xm_dispatch_emit_x64_gp_rr_comm(XM_AND, &buf, X64_RAX, X64_RBX))
        return 1;
    if (!xm_dispatch_emit_x64_gp_rr_comm(XM_OR, &buf, X64_RCX, X64_RDX))
        return 1;
    if (!xm_dispatch_emit_x64_gp_rr_comm(XM_XOR, &buf, X64_RSI, X64_RDI))
        return 1;
    if (buf.pos == 0)
        return 1;
    uint32_t pos_before = buf.pos;
    if (xm_dispatch_emit_x64_gp_rr(XM_ADD, &buf, X64_RAX, X64_RBX))
        return 1;
    if (buf.pos != pos_before)
        return 1;
    if (xm_dispatch_emit_x64_gp_rr_comm(XM_SUB, &buf, X64_RAX, X64_RBX))
        return 1;
    if (buf.pos != pos_before)
        return 1;
    return 0;
}

static int test_x64_cmp_rr_cc(void) {
    uint8_t code[128];
    uint8_t expected[128];
    X64Buf buf;
    X64Buf exp;
    memset(code, 0, sizeof(code));
    memset(expected, 0, sizeof(expected));
    x64_buf_init(&buf, code, (uint32_t) sizeof(code));
    x64_buf_init(&exp, expected, (uint32_t) sizeof(expected));

    if (!xm_dispatch_emit_x64_cmp_rr_cc(XM_EQ, &buf, X64_RAX, X64_RBX, X64_RCX))
        return 1;
    x64_cmp_rr(&exp, X64_RBX, X64_RCX);
    x64_mov_ri32(&exp, X64_RAX, 0);
    x64_setcc(&exp, X64_CC_E, X64_RAX);
    if (!xm_dispatch_emit_x64_cmp_rr_cc(XM_NE, &buf, X64_RDX, X64_RSI, X64_RDI))
        return 1;
    x64_cmp_rr(&exp, X64_RSI, X64_RDI);
    x64_mov_ri32(&exp, X64_RDX, 0);
    x64_setcc(&exp, X64_CC_NE, X64_RDX);
    if (!xm_dispatch_emit_x64_cmp_rr_cc(XM_LT, &buf, X64_R8, X64_R9, X64_R10))
        return 1;
    x64_cmp_rr(&exp, X64_R9, X64_R10);
    x64_mov_ri32(&exp, X64_R8, 0);
    x64_setcc(&exp, X64_CC_L, X64_R8);
    if (!xm_dispatch_emit_x64_cmp_rr_cc(XM_LE, &buf, X64_R11, X64_R12, X64_R13))
        return 1;
    x64_cmp_rr(&exp, X64_R12, X64_R13);
    x64_mov_ri32(&exp, X64_R11, 0);
    x64_setcc(&exp, X64_CC_LE, X64_R11);
    if (!xm_dispatch_emit_x64_cmp_rr_cc(XM_GT, &buf, X64_R14, X64_R15, X64_RAX))
        return 1;
    x64_cmp_rr(&exp, X64_R15, X64_RAX);
    x64_mov_ri32(&exp, X64_R14, 0);
    x64_setcc(&exp, X64_CC_G, X64_R14);
    if (!xm_dispatch_emit_x64_cmp_rr_cc(XM_GE, &buf, X64_RCX, X64_RDX, X64_R8))
        return 1;
    x64_cmp_rr(&exp, X64_RDX, X64_R8);
    x64_mov_ri32(&exp, X64_RCX, 0);
    x64_setcc(&exp, X64_CC_GE, X64_RCX);

    if (buf.pos != exp.pos)
        return 1;
    if (memcmp(code, expected, buf.pos) != 0)
        return 1;
    uint32_t pos_before = buf.pos;
    if (xm_dispatch_emit_x64_cmp_rr_cc(XM_ADD, &buf, X64_RAX, X64_RBX, X64_RCX))
        return 1;
    if (xm_dispatch_emit_x64_cmp_rr_cc(XM_FEQ, &buf, X64_RAX, X64_RBX, X64_RCX))
        return 1;
    if (buf.pos != pos_before)
        return 1;
    return 0;
}
#endif

#ifdef __aarch64__
static int test_arm64_gp_rrr(void) {
    uint32_t code[8] = {0};
    A64Buf buf;
    a64_buf_init(&buf, code, 8);

    if (!xm_dispatch_emit_arm64_gp_rrr(XM_ADD, &buf, A64_X0, A64_X1, A64_X2))
        return 1;
    if (!xm_dispatch_emit_arm64_gp_rrr(XM_SUB, &buf, A64_X3, A64_X4, A64_X5))
        return 1;
    if (!xm_dispatch_emit_arm64_gp_rrr(XM_MUL, &buf, A64_X6, A64_X7, A64_X8))
        return 1;
    if (!xm_dispatch_emit_arm64_gp_rrr(XM_AND, &buf, A64_X0, A64_X1, A64_X2))
        return 1;
    if (!xm_dispatch_emit_arm64_gp_rrr(XM_OR, &buf, A64_X3, A64_X4, A64_X5))
        return 1;
    if (!xm_dispatch_emit_arm64_gp_rrr(XM_XOR, &buf, A64_X6, A64_X7, A64_X8))
        return 1;
    if (!xm_dispatch_emit_arm64_gp_rrr(XM_SHL, &buf, A64_X0, A64_X1, A64_X2))
        return 1;
    if (!xm_dispatch_emit_arm64_gp_rrr(XM_SHR, &buf, A64_X3, A64_X4, A64_X5))
        return 1;
    if (buf.count != 8)
        return 1;
    if (code[0] != a64_add(A64_X0, A64_X1, A64_X2))
        return 1;
    if (code[1] != a64_sub(A64_X3, A64_X4, A64_X5))
        return 1;
    if (code[2] != a64_mul(A64_X6, A64_X7, A64_X8))
        return 1;
    if (code[3] != a64_and(A64_X0, A64_X1, A64_X2))
        return 1;
    if (code[4] != a64_orr(A64_X3, A64_X4, A64_X5))
        return 1;
    if (code[5] != a64_eor(A64_X6, A64_X7, A64_X8))
        return 1;
    if (code[6] != a64_lsl(A64_X0, A64_X1, A64_X2))
        return 1;
    if (code[7] != a64_asr(A64_X3, A64_X4, A64_X5))
        return 1;
    if (xm_dispatch_emit_arm64_gp_rrr(XM_DIV, &buf, A64_X0, A64_X1, A64_X2))
        return 1;
    if (buf.count != 8)
        return 1;
    return 0;
}

static int test_arm64_cmp_rr_cc(void) {
    uint32_t code[12] = {0};
    A64Buf buf;
    a64_buf_init(&buf, code, 12);

    if (!xm_dispatch_emit_arm64_cmp_rr_cc(XM_EQ, &buf, A64_X0, A64_X1, A64_X2))
        return 1;
    if (!xm_dispatch_emit_arm64_cmp_rr_cc(XM_NE, &buf, A64_X3, A64_X4, A64_X5))
        return 1;
    if (!xm_dispatch_emit_arm64_cmp_rr_cc(XM_LT, &buf, A64_X6, A64_X7, A64_X8))
        return 1;
    if (!xm_dispatch_emit_arm64_cmp_rr_cc(XM_LE, &buf, A64_X9, A64_X10, A64_X11))
        return 1;
    if (!xm_dispatch_emit_arm64_cmp_rr_cc(XM_GT, &buf, A64_X12, A64_X13, A64_X14))
        return 1;
    if (!xm_dispatch_emit_arm64_cmp_rr_cc(XM_GE, &buf, A64_X15, A64_X16, A64_X17))
        return 1;
    if (buf.count != 12)
        return 1;
    if (code[0] != a64_cmp(A64_X1, A64_X2))
        return 1;
    if (code[1] != a64_cset(A64_X0, A64_CC_EQ))
        return 1;
    if (code[2] != a64_cmp(A64_X4, A64_X5))
        return 1;
    if (code[3] != a64_cset(A64_X3, A64_CC_NE))
        return 1;
    if (code[4] != a64_cmp(A64_X7, A64_X8))
        return 1;
    if (code[5] != a64_cset(A64_X6, A64_CC_LT))
        return 1;
    if (code[6] != a64_cmp(A64_X10, A64_X11))
        return 1;
    if (code[7] != a64_cset(A64_X9, A64_CC_LE))
        return 1;
    if (code[8] != a64_cmp(A64_X13, A64_X14))
        return 1;
    if (code[9] != a64_cset(A64_X12, A64_CC_GT))
        return 1;
    if (code[10] != a64_cmp(A64_X16, A64_X17))
        return 1;
    if (code[11] != a64_cset(A64_X15, A64_CC_GE))
        return 1;
    if (xm_dispatch_emit_arm64_cmp_rr_cc(XM_ADD, &buf, A64_X0, A64_X1, A64_X2))
        return 1;
    if (xm_dispatch_emit_arm64_cmp_rr_cc(XM_FEQ, &buf, A64_X0, A64_X1, A64_X2))
        return 1;
    if (buf.count != 12)
        return 1;
    return 0;
}

static int test_arm64_fcmp_rr_cc(void) {
    uint32_t code[16] = {0};
    A64Buf buf;
    a64_buf_init(&buf, code, 16);

    if (!xm_dispatch_emit_arm64_fcmp_rr_cc(XM_FEQ, &buf, A64_X0, A64_X0, A64_X1, A64_X2))
        return 1;
    if (!xm_dispatch_emit_arm64_fcmp_rr_cc(XM_FNE, &buf, A64_X3, A64_X3, A64_X4, A64_X5))
        return 1;
    if (!xm_dispatch_emit_arm64_fcmp_rr_cc(XM_FLT, &buf, A64_X6, A64_X6, A64_X7, A64_X8))
        return 1;
    if (!xm_dispatch_emit_arm64_fcmp_rr_cc(XM_FLE, &buf, A64_X9, A64_X9, A64_X10, A64_X11))
        return 1;
    if (buf.count != 16)
        return 1;
    if (code[0] != a64_fcmp(A64_X0, A64_X1))
        return 1;
    if (code[1] != a64_cset(A64_X0, A64_CC_EQ))
        return 1;
    if (code[2] != a64_cset(A64_X2, A64_CC_VC))
        return 1;
    if (code[3] != a64_and(A64_X0, A64_X2, A64_X0))
        return 1;
    if (code[4] != a64_fcmp(A64_X3, A64_X4))
        return 1;
    if (code[5] != a64_cset(A64_X3, A64_CC_NE))
        return 1;
    if (code[6] != a64_cset(A64_X5, A64_CC_VS))
        return 1;
    if (code[7] != a64_orr(A64_X3, A64_X5, A64_X3))
        return 1;
    if (code[8] != a64_fcmp(A64_X6, A64_X7))
        return 1;
    if (code[9] != a64_cset(A64_X6, A64_CC_MI))
        return 1;
    if (code[10] != a64_cset(A64_X8, A64_CC_VC))
        return 1;
    if (code[11] != a64_and(A64_X6, A64_X8, A64_X6))
        return 1;
    if (code[12] != a64_fcmp(A64_X9, A64_X10))
        return 1;
    if (code[13] != a64_cset(A64_X9, A64_CC_LS))
        return 1;
    if (code[14] != a64_cset(A64_X11, A64_CC_VC))
        return 1;
    if (code[15] != a64_and(A64_X9, A64_X11, A64_X9))
        return 1;
    if (xm_dispatch_emit_arm64_fcmp_rr_cc(XM_ADD, &buf, A64_X0, A64_X0, A64_X1, A64_X2))
        return 1;
    if (buf.count != 16)
        return 1;
    return 0;
}
#endif

static int test_riscv64_fp_rrr(void) {
    uint32_t code[4] = {0};
    Rv64Buf buf;
    rv64_buf_init(&buf, code, 4);

    if (!xm_dispatch_emit_riscv64_fp_rrr(XM_FADD, &buf, RV64_FA0, RV64_FA1, RV64_FA2))
        return 1;
    if (!xm_dispatch_emit_riscv64_fp_rrr(XM_FSUB, &buf, RV64_FA3, RV64_FA4, RV64_FA5))
        return 1;
    if (!xm_dispatch_emit_riscv64_fp_rrr(XM_FMUL, &buf, RV64_FA6, RV64_FA7, RV64_FT0))
        return 1;
    if (!xm_dispatch_emit_riscv64_fp_rrr(XM_FDIV, &buf, RV64_FT1, RV64_FT2, RV64_FT3))
        return 1;
    if (buf.count != 4)
        return 1;
    if (code[0] != rv64_fadd_d(RV64_FA0, RV64_FA1, RV64_FA2))
        return 1;
    if (code[1] != rv64_fsub_d(RV64_FA3, RV64_FA4, RV64_FA5))
        return 1;
    if (code[2] != rv64_fmul_d(RV64_FA6, RV64_FA7, RV64_FT0))
        return 1;
    if (code[3] != rv64_fdiv_d(RV64_FT1, RV64_FT2, RV64_FT3))
        return 1;
    if (xm_dispatch_emit_riscv64_fp_rrr(XM_ADD, &buf, RV64_FA0, RV64_FA1, RV64_FA2))
        return 1;
    if (buf.count != 4)
        return 1;
    return 0;
}

#if defined(__x86_64__) || defined(_M_X64)
static int test_x64_fp(void) {
    uint8_t code[64];
    X64Buf buf;
    memset(code, 0, sizeof(code));
    x64_buf_init(&buf, code, (uint32_t) sizeof(code));

    if (!xm_dispatch_emit_x64_fp_rr_comm(XM_FADD, &buf, X64_XMM0, X64_XMM1))
        return 1;
    if (!xm_dispatch_emit_x64_fp_rr(XM_FSUB, &buf, X64_XMM2, X64_XMM3))
        return 1;
    if (!xm_dispatch_emit_x64_fp_rr_comm(XM_FMUL, &buf, X64_XMM4, X64_XMM5))
        return 1;
    if (!xm_dispatch_emit_x64_fp_rr(XM_FDIV, &buf, X64_XMM6, X64_XMM7))
        return 1;
    if (buf.pos == 0)
        return 1;
    uint32_t pos_before = buf.pos;
    if (xm_dispatch_emit_x64_fp_rr(XM_FADD, &buf, X64_XMM0, X64_XMM1))
        return 1;
    if (buf.pos != pos_before)
        return 1;
    if (xm_dispatch_emit_x64_fp_rr_comm(XM_FSUB, &buf, X64_XMM0, X64_XMM1))
        return 1;
    if (buf.pos != pos_before)
        return 1;
    return 0;
}

static int test_x64_fcmp_rr_cc(void) {
    uint8_t code[256];
    uint8_t expected[256];
    X64Buf buf;
    X64Buf exp;
    memset(code, 0, sizeof(code));
    memset(expected, 0, sizeof(expected));
    x64_buf_init(&buf, code, (uint32_t) sizeof(code));
    x64_buf_init(&exp, expected, (uint32_t) sizeof(expected));

    if (!xm_dispatch_emit_x64_fcmp_rr_cc(XM_FEQ, &buf, X64_RAX, X64_XMM0, X64_XMM1, X64_RDX))
        return 1;
    x64_xor_rr(&exp, X64_RAX, X64_RAX);
    x64_xor_rr(&exp, X64_RDX, X64_RDX);
    x64_ucomisd(&exp, X64_XMM0, X64_XMM1);
    x64_setcc(&exp, X64_CC_E, X64_RAX);
    x64_setcc(&exp, X64_CC_NP, X64_RDX);
    x64_and_rr(&exp, X64_RAX, X64_RDX);

    if (!xm_dispatch_emit_x64_fcmp_rr_cc(XM_FNE, &buf, X64_RCX, X64_XMM2, X64_XMM3, X64_R8))
        return 1;
    x64_xor_rr(&exp, X64_RCX, X64_RCX);
    x64_xor_rr(&exp, X64_R8, X64_R8);
    x64_ucomisd(&exp, X64_XMM2, X64_XMM3);
    x64_setcc(&exp, X64_CC_NE, X64_RCX);
    x64_setcc(&exp, X64_CC_P, X64_R8);
    x64_or_rr(&exp, X64_RCX, X64_R8);

    if (!xm_dispatch_emit_x64_fcmp_rr_cc(XM_FLT, &buf, X64_R9, X64_XMM4, X64_XMM5, X64_R10))
        return 1;
    x64_xor_rr(&exp, X64_R9, X64_R9);
    x64_xor_rr(&exp, X64_R10, X64_R10);
    x64_ucomisd(&exp, X64_XMM4, X64_XMM5);
    x64_setcc(&exp, X64_CC_B, X64_R9);
    x64_setcc(&exp, X64_CC_NP, X64_R10);
    x64_and_rr(&exp, X64_R9, X64_R10);

    if (!xm_dispatch_emit_x64_fcmp_rr_cc(XM_FLE, &buf, X64_R10, X64_XMM6, X64_XMM7, X64_R12))
        return 1;
    x64_xor_rr(&exp, X64_R10, X64_R10);
    x64_xor_rr(&exp, X64_R12, X64_R12);
    x64_ucomisd(&exp, X64_XMM6, X64_XMM7);
    x64_setcc(&exp, X64_CC_BE, X64_R10);
    x64_setcc(&exp, X64_CC_NP, X64_R12);
    x64_and_rr(&exp, X64_R10, X64_R12);

    if (!xm_dispatch_emit_x64_fcmp_rr_cc(XM_FEQ, &buf, X64_R11, X64_XMM8, X64_XMM9, X64_RAX))
        return 1;
    x64_xor_rr(&exp, X64_R11, X64_R11);
    x64_xor_rr(&exp, X64_RAX, X64_RAX);
    x64_ucomisd(&exp, X64_XMM8, X64_XMM9);
    x64_setcc(&exp, X64_CC_E, X64_R11);
    x64_setcc(&exp, X64_CC_NP, X64_RAX);
    x64_and_rr(&exp, X64_R11, X64_RAX);

    if (buf.pos != exp.pos)
        return 1;
    if (memcmp(code, expected, buf.pos) != 0)
        return 1;
    uint32_t pos_before = buf.pos;
    if (xm_dispatch_emit_x64_fcmp_rr_cc(XM_EQ, &buf, X64_RAX, X64_XMM0, X64_XMM1, X64_RDX))
        return 1;
    if (xm_dispatch_emit_x64_fcmp_rr_cc(XM_FADD, &buf, X64_RAX, X64_XMM0, X64_XMM1, X64_RDX))
        return 1;
    if (buf.pos != pos_before)
        return 1;
    return 0;
}
#endif

#ifdef __aarch64__
static int test_arm64_fp_rrr(void) {
    uint32_t code[4] = {0};
    A64Buf buf;
    a64_buf_init(&buf, code, 4);

    if (!xm_dispatch_emit_arm64_fp_rrr(XM_FADD, &buf, A64_X0, A64_X1, A64_X2))
        return 1;
    if (!xm_dispatch_emit_arm64_fp_rrr(XM_FSUB, &buf, A64_X3, A64_X4, A64_X5))
        return 1;
    if (!xm_dispatch_emit_arm64_fp_rrr(XM_FMUL, &buf, A64_X6, A64_X7, A64_X8))
        return 1;
    if (!xm_dispatch_emit_arm64_fp_rrr(XM_FDIV, &buf, A64_X9, A64_X10, A64_X11))
        return 1;
    if (buf.count != 4)
        return 1;
    if (code[0] != a64_fadd(A64_X0, A64_X1, A64_X2))
        return 1;
    if (code[1] != a64_fsub(A64_X3, A64_X4, A64_X5))
        return 1;
    if (code[2] != a64_fmul(A64_X6, A64_X7, A64_X8))
        return 1;
    if (code[3] != a64_fdiv(A64_X9, A64_X10, A64_X11))
        return 1;
    if (xm_dispatch_emit_arm64_fp_rrr(XM_ADD, &buf, A64_X0, A64_X1, A64_X2))
        return 1;
    if (buf.count != 4)
        return 1;
    return 0;
}
#endif

static int test_riscv64_fp_r(void) {
    uint32_t code[1] = {0};
    Rv64Buf buf;
    rv64_buf_init(&buf, code, 1);

    if (!xm_dispatch_emit_riscv64_fp_r(XM_FNEG, &buf, RV64_FA0, RV64_FA1))
        return 1;
    if (buf.count != 1)
        return 1;
    if (code[0] != rv64_fneg_d(RV64_FA0, RV64_FA1))
        return 1;
    if (xm_dispatch_emit_riscv64_fp_r(XM_FADD, &buf, RV64_FA0, RV64_FA1))
        return 1;
    if (buf.count != 1)
        return 1;
    return 0;
}

static int test_riscv64_fp_cmp_rrr(void) {
    uint32_t code[8] = {0};
    Rv64Buf buf;
    rv64_buf_init(&buf, code, 8);

    if (!xm_dispatch_emit_riscv64_fp_cmp_rrr(XM_FEQ, &buf, RV64_A0, RV64_FA1, RV64_FA2))
        return 1;
    if (!xm_dispatch_emit_riscv64_fp_cmp_rrr(XM_FNE, &buf, RV64_A3, RV64_FA4, RV64_FA5))
        return 1;
    if (!xm_dispatch_emit_riscv64_fp_cmp_rrr(XM_FLT, &buf, RV64_A6, RV64_FA0, RV64_FA1))
        return 1;
    if (!xm_dispatch_emit_riscv64_fp_cmp_rrr(XM_FLE, &buf, RV64_A7, RV64_FA2, RV64_FA3))
        return 1;
    if (buf.count != 5)
        return 1;
    if (code[0] != rv64_feq_d(RV64_A0, RV64_FA1, RV64_FA2))
        return 1;
    if (code[1] != rv64_feq_d(RV64_A3, RV64_FA4, RV64_FA5))
        return 1;
    if (code[2] != rv64_xori(RV64_A3, RV64_A3, 1))
        return 1;
    if (code[3] != rv64_flt_d(RV64_A6, RV64_FA0, RV64_FA1))
        return 1;
    if (code[4] != rv64_fle_d(RV64_A7, RV64_FA2, RV64_FA3))
        return 1;
    /* Reject case: wrapper must reject unsupported ops. */
    if (xm_dispatch_emit_riscv64_fp_cmp_rrr(XM_ADD, &buf, RV64_A0, RV64_FA1, RV64_FA2))
        return 1;
    if (buf.count != 5)
        return 1;
    return 0;
}

static int test_riscv64_gp_cmp_inv_rrr(void) {
    /* LE/GE: each emits slt + xori (2 words). Both wrappers share the body. */
    uint32_t code[4] = {0};
    Rv64Buf buf;
    rv64_buf_init(&buf, code, 4);

    /* GE: slt(A0, A1, A2); xori(A0, A0, 1) */
    if (!xm_dispatch_emit_riscv64_gp_cmp_inv_rrr(XM_GE, &buf, RV64_A0, RV64_A1, RV64_A2))
        return 1;
    /* LE: driver swaps rs1/rs2 — wrapper body identical. */
    if (!xm_dispatch_emit_riscv64_gp_cmp_inv_rrr(XM_LE, &buf, RV64_A3, RV64_A5, RV64_A4))
        return 1;
    if (buf.count != 4)
        return 1;
    if (code[0] != rv64_slt(RV64_A0, RV64_A1, RV64_A2))
        return 1;
    if (code[1] != rv64_xori(RV64_A0, RV64_A0, 1))
        return 1;
    if (code[2] != rv64_slt(RV64_A3, RV64_A5, RV64_A4))
        return 1;
    if (code[3] != rv64_xori(RV64_A3, RV64_A3, 1))
        return 1;
    /* Non-covered ops must reject without writing. */
    if (xm_dispatch_emit_riscv64_gp_cmp_inv_rrr(XM_LT, &buf, RV64_A0, RV64_A1, RV64_A2))
        return 1;
    if (xm_dispatch_emit_riscv64_gp_cmp_inv_rrr(XM_EQ, &buf, RV64_A0, RV64_A1, RV64_A2))
        return 1;
    if (buf.count != 4)
        return 1;
    return 0;
}

static int test_riscv64_gp_cmp_diff_rrr(void) {
    /* EQ: sub(scratch, rs1, rs2); sltiu(rd, scratch, 1).
     * NE: sub(scratch, rs1, rs2); sltu(rd, X0, scratch). */
    uint32_t code[4] = {0};
    Rv64Buf buf;
    rv64_buf_init(&buf, code, 4);

    if (!xm_dispatch_emit_riscv64_gp_cmp_diff_rrr(XM_EQ, &buf, RV64_A0, RV64_A1, RV64_A2, RV64_T0))
        return 1;
    if (!xm_dispatch_emit_riscv64_gp_cmp_diff_rrr(XM_NE, &buf, RV64_A3, RV64_A4, RV64_A5, RV64_T1))
        return 1;
    if (buf.count != 4)
        return 1;
    if (code[0] != rv64_sub(RV64_T0, RV64_A1, RV64_A2))
        return 1;
    if (code[1] != rv64_sltiu(RV64_A0, RV64_T0, 1))
        return 1;
    if (code[2] != rv64_sub(RV64_T1, RV64_A4, RV64_A5))
        return 1;
    if (code[3] != rv64_sltu(RV64_A3, RV64_X0, RV64_T1))
        return 1;
    /* Non-covered ops must reject without writing. */
    if (xm_dispatch_emit_riscv64_gp_cmp_diff_rrr(XM_LT, &buf, RV64_A0, RV64_A1, RV64_A2, RV64_T0))
        return 1;
    if (xm_dispatch_emit_riscv64_gp_cmp_diff_rrr(XM_GE, &buf, RV64_A0, RV64_A1, RV64_A2, RV64_T0))
        return 1;
    if (buf.count != 4)
        return 1;
    return 0;
}

static int test_riscv64_conv_i2f(void) {
    uint32_t code[1] = {0};
    Rv64Buf buf;
    rv64_buf_init(&buf, code, 1);

    if (!xm_dispatch_emit_riscv64_conv_i2f(XM_I2F, &buf, RV64_FA0, RV64_A1))
        return 1;
    if (buf.count != 1)
        return 1;
    if (code[0] != rv64_fcvt_d_l(RV64_FA0, RV64_A1))
        return 1;
    if (xm_dispatch_emit_riscv64_conv_i2f(XM_F2I, &buf, RV64_FA0, RV64_A1))
        return 1;
    if (buf.count != 1)
        return 1;
    return 0;
}

static int test_riscv64_conv_f2i(void) {
    uint32_t code[1] = {0};
    Rv64Buf buf;
    rv64_buf_init(&buf, code, 1);

    if (!xm_dispatch_emit_riscv64_conv_f2i(XM_F2I, &buf, RV64_A0, RV64_FA1))
        return 1;
    if (buf.count != 1)
        return 1;
    if (code[0] != rv64_fcvt_l_d(RV64_A0, RV64_FA1))
        return 1;
    if (xm_dispatch_emit_riscv64_conv_f2i(XM_I2F, &buf, RV64_A0, RV64_FA1))
        return 1;
    if (buf.count != 1)
        return 1;
    return 0;
}

static int test_riscv64_mem_subword(void) {
    uint32_t code[9] = {0};
    Rv64Buf buf;
    rv64_buf_init(&buf, code, 9);

    if (!xm_dispatch_emit_riscv64_mem_load_gp(XM_LOAD8Z, &buf, RV64_A0, RV64_A1, 1))
        return 1;
    if (!xm_dispatch_emit_riscv64_mem_load_gp(XM_LOAD8S, &buf, RV64_A2, RV64_A3, 2))
        return 1;
    if (!xm_dispatch_emit_riscv64_mem_load_gp(XM_LOAD16Z, &buf, RV64_A4, RV64_A5, 4))
        return 1;
    if (!xm_dispatch_emit_riscv64_mem_load_gp(XM_LOAD16S, &buf, RV64_A6, RV64_A7, 6))
        return 1;
    if (!xm_dispatch_emit_riscv64_mem_load_gp(XM_LOAD32Z, &buf, RV64_T0, RV64_T1, 8))
        return 1;
    if (!xm_dispatch_emit_riscv64_mem_load_gp(XM_LOAD32S, &buf, RV64_T2, RV64_T3, 12))
        return 1;
    if (!xm_dispatch_emit_riscv64_mem_store_gp(XM_STORE8, &buf, RV64_A1, 1, RV64_A0))
        return 1;
    if (!xm_dispatch_emit_riscv64_mem_store_gp(XM_STORE16, &buf, RV64_A3, 2, RV64_A2))
        return 1;
    if (!xm_dispatch_emit_riscv64_mem_store_gp(XM_STORE32, &buf, RV64_A5, 4, RV64_A4))
        return 1;
    if (buf.count != 9)
        return 1;
    if (code[0] != rv64_lbu(RV64_A0, RV64_A1, 1))
        return 1;
    if (code[1] != rv64_lb(RV64_A2, RV64_A3, 2))
        return 1;
    if (code[2] != rv64_lhu(RV64_A4, RV64_A5, 4))
        return 1;
    if (code[3] != rv64_lh(RV64_A6, RV64_A7, 6))
        return 1;
    if (code[4] != rv64_lwu(RV64_T0, RV64_T1, 8))
        return 1;
    if (code[5] != rv64_lw(RV64_T2, RV64_T3, 12))
        return 1;
    if (code[6] != rv64_sb(RV64_A0, RV64_A1, 1))
        return 1;
    if (code[7] != rv64_sh(RV64_A2, RV64_A3, 2))
        return 1;
    if (code[8] != rv64_sw(RV64_A4, RV64_A5, 4))
        return 1;
    if (xm_dispatch_emit_riscv64_mem_load_gp(XM_STORE8, &buf, RV64_A0, RV64_A1, 0))
        return 1;
    if (xm_dispatch_emit_riscv64_mem_store_gp(XM_LOAD8Z, &buf, RV64_A1, 0, RV64_A0))
        return 1;
    if (buf.count != 9)
        return 1;
    return 0;
}

#if defined(__x86_64__) || defined(_M_X64)
static int test_x64_gp_r(void) {
    uint8_t code[16];
    X64Buf buf;
    memset(code, 0, sizeof(code));
    x64_buf_init(&buf, code, (uint32_t) sizeof(code));

    if (!xm_dispatch_emit_x64_gp_r(XM_NOT, &buf, X64_RAX))
        return 1;
    if (!xm_dispatch_emit_x64_gp_r(XM_NEG, &buf, X64_RBX))
        return 1;
    if (buf.pos == 0)
        return 1;
    uint32_t pos_before = buf.pos;
    if (xm_dispatch_emit_x64_gp_r(XM_ADD, &buf, X64_RAX))
        return 1;
    if (buf.pos != pos_before)
        return 1;
    return 0;
}

static int test_x64_conv_i2f(void) {
    uint8_t code[32];
    X64Buf buf;
    memset(code, 0, sizeof(code));
    x64_buf_init(&buf, code, (uint32_t) sizeof(code));

    if (!xm_dispatch_emit_x64_conv_i2f(XM_I2F, &buf, X64_XMM0, X64_RBX))
        return 1;
    if (buf.pos == 0)
        return 1;
    uint32_t pos_before = buf.pos;
    if (xm_dispatch_emit_x64_conv_i2f(XM_F2I, &buf, X64_XMM0, X64_RBX))
        return 1;
    if (buf.pos != pos_before)
        return 1;
    return 0;
}

static int test_x64_conv_f2i(void) {
    uint8_t code[32];
    X64Buf buf;
    memset(code, 0, sizeof(code));
    x64_buf_init(&buf, code, (uint32_t) sizeof(code));

    if (!xm_dispatch_emit_x64_conv_f2i(XM_F2I, &buf, X64_RAX, X64_XMM1))
        return 1;
    if (buf.pos == 0)
        return 1;
    uint32_t pos_before = buf.pos;
    if (xm_dispatch_emit_x64_conv_f2i(XM_I2F, &buf, X64_RAX, X64_XMM1))
        return 1;
    if (buf.pos != pos_before)
        return 1;
    return 0;
}

static int test_x64_mem_subword(void) {
    uint8_t code[128];
    uint8_t expected[128];
    X64Buf buf;
    X64Buf exp;
    memset(code, 0, sizeof(code));
    memset(expected, 0, sizeof(expected));
    x64_buf_init(&buf, code, (uint32_t) sizeof(code));
    x64_buf_init(&exp, expected, (uint32_t) sizeof(expected));

    if (!xm_dispatch_emit_x64_mem_load_gp(XM_LOAD8Z, &buf, X64_RAX, X64_RBX, 1))
        return 1;
    x64_movzx_rm8(&exp, X64_RAX, X64_RBX, 1);
    if (!xm_dispatch_emit_x64_mem_load_gp(XM_LOAD8S, &buf, X64_RCX, X64_RDX, 2))
        return 1;
    x64_movsx_rm8(&exp, X64_RCX, X64_RDX, 2);
    if (!xm_dispatch_emit_x64_mem_load_gp(XM_LOAD16Z, &buf, X64_RSI, X64_RDI, 4))
        return 1;
    x64_movzx_rm16(&exp, X64_RSI, X64_RDI, 4);
    if (!xm_dispatch_emit_x64_mem_load_gp(XM_LOAD16S, &buf, X64_R8, X64_R9, 6))
        return 1;
    x64_movsx_rm16(&exp, X64_R8, X64_R9, 6);
    if (!xm_dispatch_emit_x64_mem_load_gp(XM_LOAD32Z, &buf, X64_R10, X64_R11, 8))
        return 1;
    x64_mov_rm32(&exp, X64_R10, X64_R11, 8);
    if (!xm_dispatch_emit_x64_mem_load_gp(XM_LOAD32S, &buf, X64_R12, X64_R13, 12))
        return 1;
    x64_movsxd_rm(&exp, X64_R12, X64_R13, 12);
    if (!xm_dispatch_emit_x64_mem_store_gp(XM_STORE8, &buf, X64_RBX, 1, X64_RAX))
        return 1;
    x64_mov_mr8(&exp, X64_RBX, 1, X64_RAX);
    if (!xm_dispatch_emit_x64_mem_store_gp(XM_STORE16, &buf, X64_RDX, 2, X64_RCX))
        return 1;
    x64_mov_mr16(&exp, X64_RDX, 2, X64_RCX);
    if (!xm_dispatch_emit_x64_mem_store_gp(XM_STORE32, &buf, X64_RDI, 4, X64_RSI))
        return 1;
    x64_mov_mr32(&exp, X64_RDI, 4, X64_RSI);

    if (buf.pos != exp.pos)
        return 1;
    if (memcmp(code, expected, buf.pos) != 0)
        return 1;
    uint32_t pos_before = buf.pos;
    if (xm_dispatch_emit_x64_mem_load_gp(XM_STORE8, &buf, X64_RAX, X64_RBX, 0))
        return 1;
    if (xm_dispatch_emit_x64_mem_store_gp(XM_LOAD8Z, &buf, X64_RBX, 0, X64_RAX))
        return 1;
    if (buf.pos != pos_before)
        return 1;
    return 0;
}
#endif

#ifdef __aarch64__
static int test_arm64_gp_r(void) {
    uint32_t code[2] = {0};
    A64Buf buf;
    a64_buf_init(&buf, code, 2);

    if (!xm_dispatch_emit_arm64_gp_r(XM_NEG, &buf, A64_X0, A64_X1))
        return 1;
    if (!xm_dispatch_emit_arm64_gp_r(XM_NOT, &buf, A64_X2, A64_X3))
        return 1;
    if (buf.count != 2)
        return 1;
    if (code[0] != a64_neg(A64_X0, A64_X1))
        return 1;
    if (code[1] != a64_mvn(A64_X2, A64_X3))
        return 1;
    if (xm_dispatch_emit_arm64_gp_r(XM_ADD, &buf, A64_X0, A64_X1))
        return 1;
    if (buf.count != 2)
        return 1;
    return 0;
}

static int test_arm64_fp_r(void) {
    uint32_t code[1] = {0};
    A64Buf buf;
    a64_buf_init(&buf, code, 1);

    if (!xm_dispatch_emit_arm64_fp_r(XM_FNEG, &buf, A64_X0, A64_X1))
        return 1;
    if (buf.count != 1)
        return 1;
    if (code[0] != a64_fneg(A64_X0, A64_X1))
        return 1;
    if (xm_dispatch_emit_arm64_fp_r(XM_FADD, &buf, A64_X0, A64_X1))
        return 1;
    if (buf.count != 1)
        return 1;
    return 0;
}

static int test_arm64_conv_i2f(void) {
    uint32_t code[1] = {0};
    A64Buf buf;
    a64_buf_init(&buf, code, 1);

    if (!xm_dispatch_emit_arm64_conv_i2f(XM_I2F, &buf, A64_X0, A64_X1))
        return 1;
    if (buf.count != 1)
        return 1;
    if (code[0] != a64_scvtf(A64_X0, A64_X1))
        return 1;
    if (xm_dispatch_emit_arm64_conv_i2f(XM_F2I, &buf, A64_X0, A64_X1))
        return 1;
    if (buf.count != 1)
        return 1;
    return 0;
}

static int test_arm64_conv_f2i(void) {
    uint32_t code[1] = {0};
    A64Buf buf;
    a64_buf_init(&buf, code, 1);

    if (!xm_dispatch_emit_arm64_conv_f2i(XM_F2I, &buf, A64_X0, A64_X1))
        return 1;
    if (buf.count != 1)
        return 1;
    if (code[0] != a64_fcvtzs(A64_X0, A64_X1))
        return 1;
    if (xm_dispatch_emit_arm64_conv_f2i(XM_I2F, &buf, A64_X0, A64_X1))
        return 1;
    if (buf.count != 1)
        return 1;
    return 0;
}

static int test_arm64_mem_subword(void) {
    uint32_t code[9] = {0};
    A64Buf buf;
    a64_buf_init(&buf, code, 9);

    if (!xm_dispatch_emit_arm64_mem_load_gp(XM_LOAD8Z, &buf, A64_X0, A64_X1, 1))
        return 1;
    if (!xm_dispatch_emit_arm64_mem_load_gp(XM_LOAD8S, &buf, A64_X2, A64_X3, 2))
        return 1;
    if (!xm_dispatch_emit_arm64_mem_load_gp(XM_LOAD16Z, &buf, A64_X4, A64_X5, 4))
        return 1;
    if (!xm_dispatch_emit_arm64_mem_load_gp(XM_LOAD16S, &buf, A64_X6, A64_X7, 6))
        return 1;
    if (!xm_dispatch_emit_arm64_mem_load_gp(XM_LOAD32Z, &buf, A64_X8, A64_X9, 8))
        return 1;
    if (!xm_dispatch_emit_arm64_mem_load_gp(XM_LOAD32S, &buf, A64_X10, A64_X11, 12))
        return 1;
    if (!xm_dispatch_emit_arm64_mem_store_gp(XM_STORE8, &buf, A64_X1, 1, A64_X0))
        return 1;
    if (!xm_dispatch_emit_arm64_mem_store_gp(XM_STORE16, &buf, A64_X3, 2, A64_X2))
        return 1;
    if (!xm_dispatch_emit_arm64_mem_store_gp(XM_STORE32, &buf, A64_X5, 4, A64_X4))
        return 1;
    if (buf.count != 9)
        return 1;
    if (code[0] != a64_ldrb(A64_X0, A64_X1, 1))
        return 1;
    if (code[1] != a64_ldrsb(A64_X2, A64_X3, 2))
        return 1;
    if (code[2] != a64_ldrh(A64_X4, A64_X5, 4))
        return 1;
    if (code[3] != a64_ldrsh(A64_X6, A64_X7, 6))
        return 1;
    if (code[4] != a64_ldr_w(A64_X8, A64_X9, 8))
        return 1;
    if (code[5] != a64_ldrsw(A64_X10, A64_X11, 12))
        return 1;
    if (code[6] != a64_strb(A64_X0, A64_X1, 1))
        return 1;
    if (code[7] != a64_strh(A64_X2, A64_X3, 2))
        return 1;
    if (code[8] != a64_str_w(A64_X4, A64_X5, 4))
        return 1;
    if (xm_dispatch_emit_arm64_mem_load_gp(XM_STORE8, &buf, A64_X0, A64_X1, 0))
        return 1;
    if (xm_dispatch_emit_arm64_mem_store_gp(XM_LOAD8Z, &buf, A64_X1, 0, A64_X0))
        return 1;
    if (buf.count != 9)
        return 1;
    return 0;
}
#endif

int main(void) {
    if (test_riscv64_gp_rrr() != 0) {
        fprintf(stderr, "test_riscv64_gp_rrr failed\n");
        return 1;
    }
    if (test_riscv64_fp_rrr() != 0) {
        fprintf(stderr, "test_riscv64_fp_rrr failed\n");
        return 1;
    }
    if (test_riscv64_fp_r() != 0) {
        fprintf(stderr, "test_riscv64_fp_r failed\n");
        return 1;
    }
    if (test_riscv64_fp_cmp_rrr() != 0) {
        fprintf(stderr, "test_riscv64_fp_cmp_rrr failed\n");
        return 1;
    }
    if (test_riscv64_gp_cmp_inv_rrr() != 0) {
        fprintf(stderr, "test_riscv64_gp_cmp_inv_rrr failed\n");
        return 1;
    }
    if (test_riscv64_gp_cmp_diff_rrr() != 0) {
        fprintf(stderr, "test_riscv64_gp_cmp_diff_rrr failed\n");
        return 1;
    }
    if (test_riscv64_conv_i2f() != 0) {
        fprintf(stderr, "test_riscv64_conv_i2f failed\n");
        return 1;
    }
    if (test_riscv64_conv_f2i() != 0) {
        fprintf(stderr, "test_riscv64_conv_f2i failed\n");
        return 1;
    }
    if (test_riscv64_mem_subword() != 0) {
        fprintf(stderr, "test_riscv64_mem_subword failed\n");
        return 1;
    }
#if defined(__x86_64__) || defined(_M_X64)
    if (test_x64_gp_rr() != 0) {
        fprintf(stderr, "test_x64_gp_rr failed\n");
        return 1;
    }
    if (test_x64_cmp_rr_cc() != 0) {
        fprintf(stderr, "test_x64_cmp_rr_cc failed\n");
        return 1;
    }
    if (test_x64_fp() != 0) {
        fprintf(stderr, "test_x64_fp failed\n");
        return 1;
    }
    if (test_x64_fcmp_rr_cc() != 0) {
        fprintf(stderr, "test_x64_fcmp_rr_cc failed\n");
        return 1;
    }
    if (test_x64_gp_r() != 0) {
        fprintf(stderr, "test_x64_gp_r failed\n");
        return 1;
    }
    if (test_x64_conv_i2f() != 0) {
        fprintf(stderr, "test_x64_conv_i2f failed\n");
        return 1;
    }
    if (test_x64_conv_f2i() != 0) {
        fprintf(stderr, "test_x64_conv_f2i failed\n");
        return 1;
    }
    if (test_x64_mem_subword() != 0) {
        fprintf(stderr, "test_x64_mem_subword failed\n");
        return 1;
    }
#endif
#ifdef __aarch64__
    if (test_arm64_gp_rrr() != 0) {
        fprintf(stderr, "test_arm64_gp_rrr failed\n");
        return 1;
    }
    if (test_arm64_cmp_rr_cc() != 0) {
        fprintf(stderr, "test_arm64_cmp_rr_cc failed\n");
        return 1;
    }
    if (test_arm64_fcmp_rr_cc() != 0) {
        fprintf(stderr, "test_arm64_fcmp_rr_cc failed\n");
        return 1;
    }
    if (test_arm64_fp_rrr() != 0) {
        fprintf(stderr, "test_arm64_fp_rrr failed\n");
        return 1;
    }
    if (test_arm64_gp_r() != 0) {
        fprintf(stderr, "test_arm64_gp_r failed\n");
        return 1;
    }
    if (test_arm64_fp_r() != 0) {
        fprintf(stderr, "test_arm64_fp_r failed\n");
        return 1;
    }
    if (test_arm64_conv_i2f() != 0) {
        fprintf(stderr, "test_arm64_conv_i2f failed\n");
        return 1;
    }
    if (test_arm64_conv_f2i() != 0) {
        fprintf(stderr, "test_arm64_conv_f2i failed\n");
        return 1;
    }
    if (test_arm64_mem_subword() != 0) {
        fprintf(stderr, "test_arm64_mem_subword failed\n");
        return 1;
    }
#endif
    return 0;
}
