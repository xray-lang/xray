/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_x64.c - x86-64 instruction encoding implementation
 *
 * KEY CONCEPT:
 *   Each function emits one x86-64 instruction into the byte buffer.
 *   REX prefix and ModR/M edge cases (rbp/rsp) are handled by inline
 *   helpers in xir_x64.h. This file implements the typed wrappers.
 *
 * ENCODING REFERENCE:
 *   Intel SDM Vol.2, "Instruction Set Reference"
 *   AMD64 Architecture Programmer's Manual Vol.3, "General-Purpose Instructions"
 */

#ifdef __x86_64__

#include "xir_x64.h"

/* ========== Integer Arithmetic ========== */

/* ADD r64, r64:  REX.W 01 /r  (opcode direction: src→dst) */
void x64_add_rr(X64Buf *buf, X64Reg dst, X64Reg src) {
    XR_DCHECK(buf != NULL, "x64_add_rr: NULL buf");
    x64_rex_rr(buf, true, src, dst);
    x64_emit8(buf, 0x01);
    x64_modrm_rr(buf, src, dst);
}

/* ADD r64, imm32:  REX.W 81 /0 id  or  REX.W 83 /0 ib (if fits in 8 bits) */
void x64_add_ri(X64Buf *buf, X64Reg dst, int32_t imm) {
    XR_DCHECK(buf != NULL, "x64_add_ri: NULL buf");
    x64_rex(buf, true, false, false, dst > 7);
    if (imm >= -128 && imm <= 127) {
        x64_emit8(buf, 0x83);
        x64_modrm(buf, 0x3, 0, (uint8_t)dst);
        x64_emit8(buf, (uint8_t)(int8_t)imm);
    } else {
        x64_emit8(buf, 0x81);
        x64_modrm(buf, 0x3, 0, (uint8_t)dst);
        x64_emit32(buf, (uint32_t)imm);
    }
}

/* SUB r64, r64:  REX.W 29 /r */
void x64_sub_rr(X64Buf *buf, X64Reg dst, X64Reg src) {
    XR_DCHECK(buf != NULL, "x64_sub_rr: NULL buf");
    x64_rex_rr(buf, true, src, dst);
    x64_emit8(buf, 0x29);
    x64_modrm_rr(buf, src, dst);
}

/* SUB r64, imm32:  REX.W 81 /5 id  or  REX.W 83 /5 ib */
void x64_sub_ri(X64Buf *buf, X64Reg dst, int32_t imm) {
    XR_DCHECK(buf != NULL, "x64_sub_ri: NULL buf");
    x64_rex(buf, true, false, false, dst > 7);
    if (imm >= -128 && imm <= 127) {
        x64_emit8(buf, 0x83);
        x64_modrm(buf, 0x3, 5, (uint8_t)dst);
        x64_emit8(buf, (uint8_t)(int8_t)imm);
    } else {
        x64_emit8(buf, 0x81);
        x64_modrm(buf, 0x3, 5, (uint8_t)dst);
        x64_emit32(buf, (uint32_t)imm);
    }
}

/* IMUL r64, r64:  REX.W 0F AF /r */
void x64_imul_rr(X64Buf *buf, X64Reg dst, X64Reg src) {
    XR_DCHECK(buf != NULL, "x64_imul_rr: NULL buf");
    x64_rex_rr(buf, true, dst, src);
    x64_emit8(buf, 0x0F);
    x64_emit8(buf, 0xAF);
    x64_modrm_rr(buf, dst, src);
}

/* NEG r64:  REX.W F7 /3 */
void x64_neg_r(X64Buf *buf, X64Reg dst) {
    XR_DCHECK(buf != NULL, "x64_neg_r: NULL buf");
    x64_rex(buf, true, false, false, dst > 7);
    x64_emit8(buf, 0xF7);
    x64_modrm(buf, 0x3, 3, (uint8_t)dst);
}

/* CQO:  REX.W 99 */
void x64_cqo(X64Buf *buf) {
    XR_DCHECK(buf != NULL, "x64_cqo: NULL buf");
    x64_rex(buf, true, false, false, false);
    x64_emit8(buf, 0x99);
}

/* IDIV r64:  REX.W F7 /7 */
void x64_idiv_r(X64Buf *buf, X64Reg src) {
    XR_DCHECK(buf != NULL, "x64_idiv_r: NULL buf");
    x64_rex(buf, true, false, false, src > 7);
    x64_emit8(buf, 0xF7);
    x64_modrm(buf, 0x3, 7, (uint8_t)src);
}

/* ========== Logical ========== */

/* AND r64, r64:  REX.W 21 /r */
void x64_and_rr(X64Buf *buf, X64Reg dst, X64Reg src) {
    XR_DCHECK(buf != NULL, "x64_and_rr: NULL buf");
    x64_rex_rr(buf, true, src, dst);
    x64_emit8(buf, 0x21);
    x64_modrm_rr(buf, src, dst);
}

/* OR r64, r64:  REX.W 09 /r */
void x64_or_rr(X64Buf *buf, X64Reg dst, X64Reg src) {
    XR_DCHECK(buf != NULL, "x64_or_rr: NULL buf");
    x64_rex_rr(buf, true, src, dst);
    x64_emit8(buf, 0x09);
    x64_modrm_rr(buf, src, dst);
}

/* XOR r64, r64:  REX.W 31 /r */
void x64_xor_rr(X64Buf *buf, X64Reg dst, X64Reg src) {
    XR_DCHECK(buf != NULL, "x64_xor_rr: NULL buf");
    x64_rex_rr(buf, true, src, dst);
    x64_emit8(buf, 0x31);
    x64_modrm_rr(buf, src, dst);
}

/* NOT r64:  REX.W F7 /2 */
void x64_not_r(X64Buf *buf, X64Reg dst) {
    XR_DCHECK(buf != NULL, "x64_not_r: NULL buf");
    x64_rex(buf, true, false, false, dst > 7);
    x64_emit8(buf, 0xF7);
    x64_modrm(buf, 0x3, 2, (uint8_t)dst);
}

/* SHL r64, cl:  REX.W D3 /4 */
void x64_shl_rcl(X64Buf *buf, X64Reg dst) {
    XR_DCHECK(buf != NULL, "x64_shl_rcl: NULL buf");
    x64_rex(buf, true, false, false, dst > 7);
    x64_emit8(buf, 0xD3);
    x64_modrm(buf, 0x3, 4, (uint8_t)dst);
}

/* SHR r64, cl:  REX.W D3 /5 */
void x64_shr_rcl(X64Buf *buf, X64Reg dst) {
    XR_DCHECK(buf != NULL, "x64_shr_rcl: NULL buf");
    x64_rex(buf, true, false, false, dst > 7);
    x64_emit8(buf, 0xD3);
    x64_modrm(buf, 0x3, 5, (uint8_t)dst);
}

/* SAR r64, cl:  REX.W D3 /7 */
void x64_sar_rcl(X64Buf *buf, X64Reg dst) {
    XR_DCHECK(buf != NULL, "x64_sar_rcl: NULL buf");
    x64_rex(buf, true, false, false, dst > 7);
    x64_emit8(buf, 0xD3);
    x64_modrm(buf, 0x3, 7, (uint8_t)dst);
}

/* ========== Compare / Test ========== */

/* CMP r64, r64:  REX.W 39 /r */
void x64_cmp_rr(X64Buf *buf, X64Reg dst, X64Reg src) {
    XR_DCHECK(buf != NULL, "x64_cmp_rr: NULL buf");
    x64_rex_rr(buf, true, src, dst);
    x64_emit8(buf, 0x39);
    x64_modrm_rr(buf, src, dst);
}

/* CMP r64, imm32:  REX.W 81 /7 id  or  REX.W 83 /7 ib */
void x64_cmp_ri(X64Buf *buf, X64Reg dst, int32_t imm) {
    XR_DCHECK(buf != NULL, "x64_cmp_ri: NULL buf");
    x64_rex(buf, true, false, false, dst > 7);
    if (imm >= -128 && imm <= 127) {
        x64_emit8(buf, 0x83);
        x64_modrm(buf, 0x3, 7, (uint8_t)dst);
        x64_emit8(buf, (uint8_t)(int8_t)imm);
    } else {
        x64_emit8(buf, 0x81);
        x64_modrm(buf, 0x3, 7, (uint8_t)dst);
        x64_emit32(buf, (uint32_t)imm);
    }
}

/* TEST r64, r64:  REX.W 85 /r */
void x64_test_rr(X64Buf *buf, X64Reg dst, X64Reg src) {
    XR_DCHECK(buf != NULL, "x64_test_rr: NULL buf");
    x64_rex_rr(buf, true, src, dst);
    x64_emit8(buf, 0x85);
    x64_modrm_rr(buf, src, dst);
}

/* ========== Move ========== */

/* MOV r64, r64:  REX.W 89 /r */
void x64_mov_rr(X64Buf *buf, X64Reg dst, X64Reg src) {
    XR_DCHECK(buf != NULL, "x64_mov_rr: NULL buf");
    x64_rex_rr(buf, true, src, dst);
    x64_emit8(buf, 0x89);
    x64_modrm_rr(buf, src, dst);
}

/* MOV r64, imm64:  REX.W B8+rd io */
void x64_mov_ri64(X64Buf *buf, X64Reg dst, uint64_t imm) {
    XR_DCHECK(buf != NULL, "x64_mov_ri64: NULL buf");
    x64_rex(buf, true, false, false, dst > 7);
    x64_emit8(buf, 0xB8 + ((uint8_t)dst & 7));
    x64_emit64(buf, imm);
}

/* MOV r32, imm32:  B8+rd id (zero-extends to 64 bits, no REX.W) */
void x64_mov_ri32(X64Buf *buf, X64Reg dst, uint32_t imm) {
    XR_DCHECK(buf != NULL, "x64_mov_ri32: NULL buf");
    if (dst > 7)
        x64_rex(buf, false, false, false, true);
    x64_emit8(buf, 0xB8 + ((uint8_t)dst & 7));
    x64_emit32(buf, imm);
}

/* MOV r64, [base + disp]:  REX.W 8B /r */
void x64_mov_rm(X64Buf *buf, X64Reg dst, X64Reg base, int32_t disp) {
    XR_DCHECK(buf != NULL, "x64_mov_rm: NULL buf");
    x64_rex(buf, true, dst > 7, false, base > 7);
    x64_emit8(buf, 0x8B);
    x64_modrm_mem(buf, dst, base, disp);
}

/* MOV [base + disp], r64:  REX.W 89 /r */
void x64_mov_mr(X64Buf *buf, X64Reg base, int32_t disp, X64Reg src) {
    XR_DCHECK(buf != NULL, "x64_mov_mr: NULL buf");
    x64_rex(buf, true, src > 7, false, base > 7);
    x64_emit8(buf, 0x89);
    x64_modrm_mem(buf, src, base, disp);
}

/* LEA r64, [base + disp]:  REX.W 8D /r */
void x64_lea(X64Buf *buf, X64Reg dst, X64Reg base, int32_t disp) {
    XR_DCHECK(buf != NULL, "x64_lea: NULL buf");
    x64_rex(buf, true, dst > 7, false, base > 7);
    x64_emit8(buf, 0x8D);
    x64_modrm_mem(buf, dst, base, disp);
}

/* MOVSXD r64, [base + disp]:  REX.W 63 /r */
void x64_movsxd_rm(X64Buf *buf, X64Reg dst, X64Reg base, int32_t disp) {
    XR_DCHECK(buf != NULL, "x64_movsxd_rm: NULL buf");
    x64_rex(buf, true, dst > 7, false, base > 7);
    x64_emit8(buf, 0x63);
    x64_modrm_mem(buf, dst, base, disp);
}

/* CMOVcc r64, r64:  REX.W 0F 4x /r */
void x64_cmov_rr(X64Buf *buf, X64Cond cc, X64Reg dst, X64Reg src) {
    XR_DCHECK(buf != NULL, "x64_cmov_rr: NULL buf");
    x64_rex_rr(buf, true, dst, src);
    x64_emit8(buf, 0x0F);
    x64_emit8(buf, 0x40 + (uint8_t)cc);
    x64_modrm_rr(buf, dst, src);
}

/* SETcc r8:  REX 0F 9x /0 */
void x64_setcc(X64Buf *buf, X64Cond cc, X64Reg dst) {
    XR_DCHECK(buf != NULL, "x64_setcc: NULL buf");
    /* Need REX prefix for r8-r15, also to avoid accessing AH/CH/DH/BH */
    if (dst > 7 || dst >= 4)
        x64_rex(buf, false, false, false, dst > 7);
    x64_emit8(buf, 0x0F);
    x64_emit8(buf, 0x90 + (uint8_t)cc);
    x64_modrm(buf, 0x3, 0, (uint8_t)dst);
}

/* ========== Branch ========== */

/* JMP rel32:  E9 cd */
void x64_jmp_rel32(X64Buf *buf, int32_t offset) {
    XR_DCHECK(buf != NULL, "x64_jmp_rel32: NULL buf");
    x64_emit8(buf, 0xE9);
    x64_emit32(buf, (uint32_t)offset);
}

/* JMP rel8:  EB cb */
void x64_jmp_rel8(X64Buf *buf, int8_t offset) {
    XR_DCHECK(buf != NULL, "x64_jmp_rel8: NULL buf");
    x64_emit8(buf, 0xEB);
    x64_emit8(buf, (uint8_t)offset);
}

/* Jcc rel32:  0F 8x cd */
void x64_jcc_rel32(X64Buf *buf, X64Cond cc, int32_t offset) {
    XR_DCHECK(buf != NULL, "x64_jcc_rel32: NULL buf");
    x64_emit8(buf, 0x0F);
    x64_emit8(buf, 0x80 + (uint8_t)cc);
    x64_emit32(buf, (uint32_t)offset);
}

/* CALL rel32:  E8 cd */
void x64_call_rel32(X64Buf *buf, int32_t offset) {
    XR_DCHECK(buf != NULL, "x64_call_rel32: NULL buf");
    x64_emit8(buf, 0xE8);
    x64_emit32(buf, (uint32_t)offset);
}

/* CALL r64:  REX? FF /2 */
void x64_call_r(X64Buf *buf, X64Reg target) {
    XR_DCHECK(buf != NULL, "x64_call_r: NULL buf");
    if (target > 7)
        x64_rex(buf, false, false, false, true);
    x64_emit8(buf, 0xFF);
    x64_modrm(buf, 0x3, 2, (uint8_t)target);
}

/* RET:  C3 */
void x64_ret(X64Buf *buf) {
    XR_DCHECK(buf != NULL, "x64_ret: NULL buf");
    x64_emit8(buf, 0xC3);
}

/* NOP:  90 */
void x64_nop(X64Buf *buf) {
    XR_DCHECK(buf != NULL, "x64_nop: NULL buf");
    x64_emit8(buf, 0x90);
}

/* ========== Stack ========== */

/* PUSH r64:  50+rd (+ REX.B for r8-r15) */
void x64_push_r(X64Buf *buf, X64Reg src) {
    XR_DCHECK(buf != NULL, "x64_push_r: NULL buf");
    if (src > 7)
        x64_rex(buf, false, false, false, true);
    x64_emit8(buf, 0x50 + ((uint8_t)src & 7));
}

/* POP r64:  58+rd (+ REX.B for r8-r15) */
void x64_pop_r(X64Buf *buf, X64Reg dst) {
    XR_DCHECK(buf != NULL, "x64_pop_r: NULL buf");
    if (dst > 7)
        x64_rex(buf, false, false, false, true);
    x64_emit8(buf, 0x58 + ((uint8_t)dst & 7));
}

/* ========== SSE2 Helpers ========== */

/* Emit SSE prefix + REX for xmm-xmm operations */
static void x64_sse_prefix_rr(X64Buf *buf, uint8_t prefix,
                               X64Xmm dst, X64Xmm src) {
    x64_emit8(buf, prefix);
    /* REX for xmm8-xmm15 */
    bool r = (dst > 7);
    bool b = (src > 7);
    if (r || b)
        x64_rex(buf, false, r, false, b);
}

/* ADDSD xmm, xmm:  F2 0F 58 /r */
void x64_addsd(X64Buf *buf, X64Xmm dst, X64Xmm src) {
    XR_DCHECK(buf != NULL, "x64_addsd: NULL buf");
    x64_sse_prefix_rr(buf, 0xF2, dst, src);
    x64_emit8(buf, 0x0F);
    x64_emit8(buf, 0x58);
    x64_modrm(buf, 0x3, (uint8_t)dst & 7, (uint8_t)src & 7);
}

/* SUBSD xmm, xmm:  F2 0F 5C /r */
void x64_subsd(X64Buf *buf, X64Xmm dst, X64Xmm src) {
    XR_DCHECK(buf != NULL, "x64_subsd: NULL buf");
    x64_sse_prefix_rr(buf, 0xF2, dst, src);
    x64_emit8(buf, 0x0F);
    x64_emit8(buf, 0x5C);
    x64_modrm(buf, 0x3, (uint8_t)dst & 7, (uint8_t)src & 7);
}

/* MULSD xmm, xmm:  F2 0F 59 /r */
void x64_mulsd(X64Buf *buf, X64Xmm dst, X64Xmm src) {
    XR_DCHECK(buf != NULL, "x64_mulsd: NULL buf");
    x64_sse_prefix_rr(buf, 0xF2, dst, src);
    x64_emit8(buf, 0x0F);
    x64_emit8(buf, 0x59);
    x64_modrm(buf, 0x3, (uint8_t)dst & 7, (uint8_t)src & 7);
}

/* DIVSD xmm, xmm:  F2 0F 5E /r */
void x64_divsd(X64Buf *buf, X64Xmm dst, X64Xmm src) {
    XR_DCHECK(buf != NULL, "x64_divsd: NULL buf");
    x64_sse_prefix_rr(buf, 0xF2, dst, src);
    x64_emit8(buf, 0x0F);
    x64_emit8(buf, 0x5E);
    x64_modrm(buf, 0x3, (uint8_t)dst & 7, (uint8_t)src & 7);
}

/* MOVSD xmm, xmm:  F2 0F 10 /r */
void x64_movsd_rr(X64Buf *buf, X64Xmm dst, X64Xmm src) {
    XR_DCHECK(buf != NULL, "x64_movsd_rr: NULL buf");
    x64_sse_prefix_rr(buf, 0xF2, dst, src);
    x64_emit8(buf, 0x0F);
    x64_emit8(buf, 0x10);
    x64_modrm(buf, 0x3, (uint8_t)dst & 7, (uint8_t)src & 7);
}

/* MOVSD xmm, [base + disp]:  F2 REX? 0F 10 /r */
void x64_movsd_rm(X64Buf *buf, X64Xmm dst, X64Reg base, int32_t disp) {
    XR_DCHECK(buf != NULL, "x64_movsd_rm: NULL buf");
    x64_emit8(buf, 0xF2);
    bool r = ((int)dst > 7);
    bool b = (base > 7);
    if (r || b)
        x64_rex(buf, false, r, false, b);
    x64_emit8(buf, 0x0F);
    x64_emit8(buf, 0x10);
    x64_modrm_mem(buf, (X64Reg)((int)dst & 7), base, disp);
}

/* MOVSD [base + disp], xmm:  F2 REX? 0F 11 /r */
void x64_movsd_mr(X64Buf *buf, X64Reg base, int32_t disp, X64Xmm src) {
    XR_DCHECK(buf != NULL, "x64_movsd_mr: NULL buf");
    x64_emit8(buf, 0xF2);
    bool r = ((int)src > 7);
    bool b = (base > 7);
    if (r || b)
        x64_rex(buf, false, r, false, b);
    x64_emit8(buf, 0x0F);
    x64_emit8(buf, 0x11);
    x64_modrm_mem(buf, (X64Reg)((int)src & 7), base, disp);
}

/* CVTSI2SD xmm, r64:  F2 REX.W 0F 2A /r */
void x64_cvtsi2sd(X64Buf *buf, X64Xmm dst, X64Reg src) {
    XR_DCHECK(buf != NULL, "x64_cvtsi2sd: NULL buf");
    x64_emit8(buf, 0xF2);
    x64_rex(buf, true, (int)dst > 7, false, src > 7);
    x64_emit8(buf, 0x0F);
    x64_emit8(buf, 0x2A);
    x64_modrm(buf, 0x3, (uint8_t)dst & 7, (uint8_t)src & 7);
}

/* CVTTSD2SI r64, xmm:  F2 REX.W 0F 2C /r */
void x64_cvttsd2si(X64Buf *buf, X64Reg dst, X64Xmm src) {
    XR_DCHECK(buf != NULL, "x64_cvttsd2si: NULL buf");
    x64_emit8(buf, 0xF2);
    x64_rex(buf, true, dst > 7, false, (int)src > 7);
    x64_emit8(buf, 0x0F);
    x64_emit8(buf, 0x2C);
    x64_modrm(buf, 0x3, (uint8_t)dst & 7, (uint8_t)src & 7);
}

/* UCOMISD xmm, xmm:  66 0F 2E /r */
void x64_ucomisd(X64Buf *buf, X64Xmm dst, X64Xmm src) {
    XR_DCHECK(buf != NULL, "x64_ucomisd: NULL buf");
    x64_emit8(buf, 0x66);
    bool r = ((int)dst > 7);
    bool b = ((int)src > 7);
    if (r || b)
        x64_rex(buf, false, r, false, b);
    x64_emit8(buf, 0x0F);
    x64_emit8(buf, 0x2E);
    x64_modrm(buf, 0x3, (uint8_t)dst & 7, (uint8_t)src & 7);
}

/* MOVQ xmm, r64 (GP → XMM):  66 REX.W 0F 6E /r */
void x64_movq_xmm_gp(X64Buf *buf, X64Xmm dst, X64Reg src) {
    XR_DCHECK(buf != NULL, "x64_movq_xmm_gp: NULL buf");
    x64_emit8(buf, 0x66);
    x64_rex(buf, true, (int)dst > 7, false, src > 7);
    x64_emit8(buf, 0x0F);
    x64_emit8(buf, 0x6E);
    x64_modrm(buf, 0x3, (uint8_t)dst & 7, (uint8_t)src & 7);
}

/* MOVQ r64, xmm (XMM → GP):  66 REX.W 0F 7E /r */
void x64_movq_gp_xmm(X64Buf *buf, X64Reg dst, X64Xmm src) {
    XR_DCHECK(buf != NULL, "x64_movq_gp_xmm: NULL buf");
    x64_emit8(buf, 0x66);
    x64_rex(buf, true, (int)src > 7, false, dst > 7);
    x64_emit8(buf, 0x0F);
    x64_emit8(buf, 0x7E);
    x64_modrm(buf, 0x3, (uint8_t)src & 7, (uint8_t)dst & 7);
}

/* XORPD xmm, xmm:  66 0F 57 /r */
void x64_xorpd(X64Buf *buf, X64Xmm dst, X64Xmm src) {
    XR_DCHECK(buf != NULL, "x64_xorpd: NULL buf");
    x64_emit8(buf, 0x66);
    if ((int)dst > 7 || (int)src > 7)
        x64_rex(buf, false, (int)dst > 7, false, (int)src > 7);
    x64_emit8(buf, 0x0F);
    x64_emit8(buf, 0x57);
    x64_modrm(buf, 0x3, (uint8_t)dst & 7, (uint8_t)src & 7);
}

#endif // __x86_64__
