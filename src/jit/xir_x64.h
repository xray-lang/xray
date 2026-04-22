/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_x64.h - x86-64 instruction encoding for JIT code emission
 *
 * KEY CONCEPT:
 *   Byte-stream assembler for x86-64: variable-length instructions emitted
 *   into a uint8_t buffer via X64Buf. REX prefix, ModR/M, and SIB byte
 *   generation is handled by internal helpers; callers use typed wrappers.
 *
 * WHY THIS DESIGN:
 *   - x86-64 uses variable-length encoding (1-15 bytes per instruction),
 *     fundamentally different from ARM64's fixed 4-byte encoding.
 *   - Emit functions write directly to a byte buffer and advance the cursor.
 *   - Special cases (rbp needs explicit disp0, rsp needs SIB) are handled
 *     inside modrm/sib helpers, making callers simpler and less bug-prone.
 *
 * RELATED MODULES:
 *   - xir_target_x64.c: register inventory and frame layout
 *   - xir_codegen_x64.c: XIR → x86-64 machine code translation
 *   - xir_code_alloc.h: executable memory allocation
 */

#ifndef XIR_X64_H
#define XIR_X64_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "../base/xdefs.h"
#include "../base/xchecks.h"

/* ========== x86-64 Register Encoding ========== */

/*
 * Register numbers match the 4-bit encoding used in REX+ModRM/SIB.
 * Registers 0-7 need no REX extension bit; 8-15 need REX.B/R/X.
 *
 * System V AMD64 ABI:
 *   Caller-saved: rax, rcx, rdx, rsi, rdi, r8, r9, r10, r11
 *   Callee-saved: rbx, rbp, r12, r13, r14, r15
 *   Special: rsp (stack pointer)
 */
typedef enum {
    X64_RAX = 0,  X64_RCX = 1,  X64_RDX = 2,  X64_RBX = 3,
    X64_RSP = 4,  X64_RBP = 5,  X64_RSI = 6,  X64_RDI = 7,
    X64_R8  = 8,  X64_R9  = 9,  X64_R10 = 10, X64_R11 = 11,
    X64_R12 = 12, X64_R13 = 13, X64_R14 = 14, X64_R15 = 15,
} X64Reg;

/* XMM register encoding (0-15, same encoding space) */
typedef enum {
    X64_XMM0  = 0,  X64_XMM1  = 1,  X64_XMM2  = 2,  X64_XMM3  = 3,
    X64_XMM4  = 4,  X64_XMM5  = 5,  X64_XMM6  = 6,  X64_XMM7  = 7,
    X64_XMM8  = 8,  X64_XMM9  = 9,  X64_XMM10 = 10, X64_XMM11 = 11,
    X64_XMM12 = 12, X64_XMM13 = 13, X64_XMM14 = 14, X64_XMM15 = 15,
} X64Xmm;

/* ========== Condition Codes ========== */

typedef enum {
    X64_CC_O   = 0x0,  // overflow
    X64_CC_NO  = 0x1,  // no overflow
    X64_CC_B   = 0x2,  // below (unsigned <)
    X64_CC_AE  = 0x3,  // above or equal (unsigned >=)
    X64_CC_E   = 0x4,  // equal (ZF=1)
    X64_CC_NE  = 0x5,  // not equal (ZF=0)
    X64_CC_BE  = 0x6,  // below or equal (unsigned <=)
    X64_CC_A   = 0x7,  // above (unsigned >)
    X64_CC_S   = 0x8,  // sign (SF=1)
    X64_CC_NS  = 0x9,  // not sign (SF=0)
    X64_CC_P   = 0xA,  // parity (PF=1)
    X64_CC_NP  = 0xB,  // no parity (PF=0)
    X64_CC_L   = 0xC,  // less (signed <)
    X64_CC_GE  = 0xD,  // greater or equal (signed >=)
    X64_CC_LE  = 0xE,  // less or equal (signed <=)
    X64_CC_G   = 0xF,  // greater (signed >)
} X64Cond;

/* ========== Code Buffer ========== */

/*
 * Byte-stream buffer for x86-64 instructions.
 * Unlike ARM64's fixed 4-byte instructions, x86-64 uses variable-length
 * encoding. The buffer tracks byte position rather than instruction count.
 */
typedef struct {
    uint8_t  *code;      // byte buffer
    uint32_t  pos;       // current write position (bytes)
    uint32_t  capacity;  // buffer capacity (bytes)
} X64Buf;

/* Initialize buffer with externally-allocated memory */
static inline void x64_buf_init(X64Buf *buf, uint8_t *mem, uint32_t cap_bytes) {
    XR_DCHECK(buf != NULL, "x64_buf_init: NULL buf");
    XR_DCHECK(mem != NULL, "x64_buf_init: NULL mem");
    buf->code = mem;
    buf->pos = 0;
    buf->capacity = cap_bytes;
}

/* Emit a single byte */
static inline void x64_emit8(X64Buf *buf, uint8_t b) {
    XR_DCHECK(buf->pos < buf->capacity, "x64_emit8: buffer overflow");
    buf->code[buf->pos++] = b;
}

/* Emit a 32-bit little-endian immediate/displacement */
static inline void x64_emit32(X64Buf *buf, uint32_t val) {
    XR_DCHECK(buf->pos + 4 <= buf->capacity, "x64_emit32: buffer overflow");
    memcpy(&buf->code[buf->pos], &val, 4);
    buf->pos += 4;
}

/* Emit a 64-bit little-endian immediate */
static inline void x64_emit64(X64Buf *buf, uint64_t val) {
    XR_DCHECK(buf->pos + 8 <= buf->capacity, "x64_emit64: buffer overflow");
    memcpy(&buf->code[buf->pos], &val, 8);
    buf->pos += 8;
}

/* Current byte offset */
static inline uint32_t x64_buf_offset(X64Buf *buf) {
    return buf->pos;
}

/* ========== REX Prefix ========== */

/*
 * REX byte: 0100WRXB
 *   W = 1: 64-bit operand size
 *   R = 1: ModRM.reg extension (access r8-r15)
 *   X = 1: SIB.index extension (access r8-r15 as index)
 *   B = 1: ModRM.rm / SIB.base extension (access r8-r15)
 *
 * A plain REX (0x40) is needed when accessing SPL/BPL/SIL/DIL.
 * REX is only emitted when at least one bit is needed.
 */
static inline void x64_rex(X64Buf *buf, bool w, bool r, bool x, bool b) {
    uint8_t rex = 0x40;
    if (w) rex |= 0x08;
    if (r) rex |= 0x04;
    if (x) rex |= 0x02;
    if (b) rex |= 0x01;
    x64_emit8(buf, rex);
}

/* Emit REX.W prefix only if needed (64-bit + register extension bits) */
static inline void x64_rex_rr(X64Buf *buf, bool w, X64Reg reg, X64Reg rm) {
    bool r = (reg > 7);
    bool b = (rm > 7);
    if (w || r || b)
        x64_rex(buf, w, r, false, b);
}

/* ========== ModR/M + SIB Helpers ========== */

/*
 * ModR/M byte: [mod:2][reg:3][rm:3]
 *   mod=11: register direct
 *   mod=00: [rm] (no displacement, except rbp/r13 → disp32 needed)
 *   mod=01: [rm + disp8]
 *   mod=10: [rm + disp32]
 *   rm=100 (rsp/r12): SIB byte follows
 *   rm=101 with mod=00: [rip + disp32] (not [rbp])
 */
static inline void x64_modrm(X64Buf *buf, uint8_t mod, uint8_t reg, uint8_t rm) {
    x64_emit8(buf, (mod << 6) | ((reg & 7) << 3) | (rm & 7));
}

/* Register-register ModR/M: mod=11 */
static inline void x64_modrm_rr(X64Buf *buf, X64Reg reg, X64Reg rm) {
    x64_modrm(buf, 0x3, (uint8_t)reg, (uint8_t)rm);
}

/*
 * SIB byte: [scale:2][index:3][base:3]
 * Required when rm=100 in ModR/M (rsp or r12 as base).
 * scale: 0=1x, 1=2x, 2=4x, 3=8x
 */
static inline void x64_sib(X64Buf *buf, uint8_t scale, uint8_t index, uint8_t base) {
    x64_emit8(buf, (scale << 6) | ((index & 7) << 3) | (base & 7));
}

/*
 * Emit ModR/M + optional SIB + displacement for [base + disp] addressing.
 * Handles the x86-64 encoding edge cases:
 *   - rbp/r13 with disp=0 needs explicit disp8(0)
 *   - rsp/r12 as base needs SIB byte (index=rsp=100 means no index)
 */
static inline void x64_modrm_mem(X64Buf *buf, X64Reg reg, X64Reg base, int32_t disp) {
    uint8_t base3 = (uint8_t)base & 7;
    uint8_t reg3  = (uint8_t)reg & 7;
    bool need_sib = (base3 == 4);  // rsp or r12

    if (disp == 0 && base3 != 5) {
        /* mod=00: [base] no displacement */
        x64_modrm(buf, 0x0, reg3, base3);
        if (need_sib)
            x64_sib(buf, 0, 4, base3);  // index=rsp means no index
    } else if (disp >= -128 && disp <= 127) {
        /* mod=01: [base + disp8] */
        x64_modrm(buf, 0x1, reg3, base3);
        if (need_sib)
            x64_sib(buf, 0, 4, base3);
        x64_emit8(buf, (uint8_t)(int8_t)disp);
    } else {
        /* mod=10: [base + disp32] */
        x64_modrm(buf, 0x2, reg3, base3);
        if (need_sib)
            x64_sib(buf, 0, 4, base3);
        x64_emit32(buf, (uint32_t)disp);
    }
}

/* ========== Integer Arithmetic (64-bit) ========== */

/* ADD r64, r64:  REX.W 01 /r */
XR_FUNC void x64_add_rr(X64Buf *buf, X64Reg dst, X64Reg src);

/* ADD r64, imm32:  REX.W 81 /0 id */
XR_FUNC void x64_add_ri(X64Buf *buf, X64Reg dst, int32_t imm);

/* SUB r64, r64:  REX.W 29 /r */
XR_FUNC void x64_sub_rr(X64Buf *buf, X64Reg dst, X64Reg src);

/* SUB r64, imm32:  REX.W 81 /5 id */
XR_FUNC void x64_sub_ri(X64Buf *buf, X64Reg dst, int32_t imm);

/* IMUL r64, r64:  REX.W 0F AF /r */
XR_FUNC void x64_imul_rr(X64Buf *buf, X64Reg dst, X64Reg src);

/* NEG r64:  REX.W F7 /3 */
XR_FUNC void x64_neg_r(X64Buf *buf, X64Reg dst);

/* CQO (sign-extend rax into rdx:rax):  REX.W 99 */
XR_FUNC void x64_cqo(X64Buf *buf);

/* IDIV r64 (rdx:rax / r64 → rax=quot, rdx=rem):  REX.W F7 /7 */
XR_FUNC void x64_idiv_r(X64Buf *buf, X64Reg src);

/* ========== Logical ========== */

/* AND r64, r64:  REX.W 21 /r */
XR_FUNC void x64_and_rr(X64Buf *buf, X64Reg dst, X64Reg src);

/* OR r64, r64:  REX.W 09 /r */
XR_FUNC void x64_or_rr(X64Buf *buf, X64Reg dst, X64Reg src);

/* XOR r64, r64:  REX.W 31 /r */
XR_FUNC void x64_xor_rr(X64Buf *buf, X64Reg dst, X64Reg src);

/* NOT r64:  REX.W F7 /2 */
XR_FUNC void x64_not_r(X64Buf *buf, X64Reg dst);

/* SHL r64, cl:  REX.W D3 /4 */
XR_FUNC void x64_shl_rcl(X64Buf *buf, X64Reg dst);

/* SHR r64, cl:  REX.W D3 /5 */
XR_FUNC void x64_shr_rcl(X64Buf *buf, X64Reg dst);

/* SAR r64, cl:  REX.W D3 /7 */
XR_FUNC void x64_sar_rcl(X64Buf *buf, X64Reg dst);

/* ========== Compare / Test ========== */

/* CMP r64, r64:  REX.W 39 /r */
XR_FUNC void x64_cmp_rr(X64Buf *buf, X64Reg dst, X64Reg src);

/* CMP r64, imm32:  REX.W 81 /7 id */
XR_FUNC void x64_cmp_ri(X64Buf *buf, X64Reg dst, int32_t imm);

/* TEST r64, r64:  REX.W 85 /r */
XR_FUNC void x64_test_rr(X64Buf *buf, X64Reg dst, X64Reg src);

/* ========== Move ========== */

/* MOV r64, r64:  REX.W 89 /r */
XR_FUNC void x64_mov_rr(X64Buf *buf, X64Reg dst, X64Reg src);

/* MOV r64, imm64:  REX.W B8+rd io */
XR_FUNC void x64_mov_ri64(X64Buf *buf, X64Reg dst, uint64_t imm);

/* MOV r64, imm32 (zero-extended):  B8+rd id (no REX.W) */
XR_FUNC void x64_mov_ri32(X64Buf *buf, X64Reg dst, uint32_t imm);

/* MOV r64, [base + disp]:  REX.W 8B /r */
XR_FUNC void x64_mov_rm(X64Buf *buf, X64Reg dst, X64Reg base, int32_t disp);

/* MOV [base + disp], r64:  REX.W 89 /r */
XR_FUNC void x64_mov_mr(X64Buf *buf, X64Reg base, int32_t disp, X64Reg src);

/* LEA r64, [base + disp]:  REX.W 8D /r */
XR_FUNC void x64_lea(X64Buf *buf, X64Reg dst, X64Reg base, int32_t disp);

/* MOVSX r64, dword [base + disp]:  REX.W 63 /r  (MOVSXD) */
XR_FUNC void x64_movsxd_rm(X64Buf *buf, X64Reg dst, X64Reg base, int32_t disp);

/* CMOVcc r64, r64:  REX.W 0F 4x /r */
XR_FUNC void x64_cmov_rr(X64Buf *buf, X64Cond cc, X64Reg dst, X64Reg src);

/* SETcc r/m8:  0F 9x /0 (reg low byte) */
XR_FUNC void x64_setcc(X64Buf *buf, X64Cond cc, X64Reg dst);

/* ========== Branch ========== */

/* JMP rel32:  E9 cd */
XR_FUNC void x64_jmp_rel32(X64Buf *buf, int32_t offset);

/* JMP rel8:  EB cb */
XR_FUNC void x64_jmp_rel8(X64Buf *buf, int8_t offset);

/* Jcc rel32:  0F 8x cd */
XR_FUNC void x64_jcc_rel32(X64Buf *buf, X64Cond cc, int32_t offset);

/* CALL rel32:  E8 cd */
XR_FUNC void x64_call_rel32(X64Buf *buf, int32_t offset);

/* CALL r64:  FF /2 (+ REX if r8-r15) */
XR_FUNC void x64_call_r(X64Buf *buf, X64Reg target);

/* RET:  C3 */
XR_FUNC void x64_ret(X64Buf *buf);

/* NOP:  90 */
XR_FUNC void x64_nop(X64Buf *buf);

/* ========== Stack ========== */

/* PUSH r64:  50+rd (+ REX.B for r8-r15) */
XR_FUNC void x64_push_r(X64Buf *buf, X64Reg src);

/* POP r64:  58+rd (+ REX.B for r8-r15) */
XR_FUNC void x64_pop_r(X64Buf *buf, X64Reg dst);

/* ========== SSE2 Floating Point (scalar double) ========== */

/* ADDSD xmm, xmm:  F2 0F 58 /r */
XR_FUNC void x64_addsd(X64Buf *buf, X64Xmm dst, X64Xmm src);

/* SUBSD xmm, xmm:  F2 0F 5C /r */
XR_FUNC void x64_subsd(X64Buf *buf, X64Xmm dst, X64Xmm src);

/* MULSD xmm, xmm:  F2 0F 59 /r */
XR_FUNC void x64_mulsd(X64Buf *buf, X64Xmm dst, X64Xmm src);

/* DIVSD xmm, xmm:  F2 0F 5E /r */
XR_FUNC void x64_divsd(X64Buf *buf, X64Xmm dst, X64Xmm src);

/* MOVSD xmm, xmm:  F2 0F 10 /r */
XR_FUNC void x64_movsd_rr(X64Buf *buf, X64Xmm dst, X64Xmm src);

/* MOVSD xmm, [base + disp]:  F2 REX? 0F 10 /r */
XR_FUNC void x64_movsd_rm(X64Buf *buf, X64Xmm dst, X64Reg base, int32_t disp);

/* MOVSD [base + disp], xmm:  F2 REX? 0F 11 /r */
XR_FUNC void x64_movsd_mr(X64Buf *buf, X64Reg base, int32_t disp, X64Xmm src);

/* CVTSI2SD xmm, r64:  F2 REX.W 0F 2A /r */
XR_FUNC void x64_cvtsi2sd(X64Buf *buf, X64Xmm dst, X64Reg src);

/* CVTTSD2SI r64, xmm:  F2 REX.W 0F 2C /r */
XR_FUNC void x64_cvttsd2si(X64Buf *buf, X64Reg dst, X64Xmm src);

/* UCOMISD xmm, xmm:  66 0F 2E /r */
XR_FUNC void x64_ucomisd(X64Buf *buf, X64Xmm dst, X64Xmm src);

/* MOVQ xmm, r64 (GP → XMM):  66 REX.W 0F 6E /r */
XR_FUNC void x64_movq_xmm_gp(X64Buf *buf, X64Xmm dst, X64Reg src);

/* MOVQ r64, xmm (XMM → GP):  66 REX.W 0F 7E /r */
XR_FUNC void x64_movq_gp_xmm(X64Buf *buf, X64Reg dst, X64Xmm src);

/* ========== Patch Helpers ========== */

/* Patch a rel32 at the given byte offset to jump to target_offset */
static inline void x64_patch_rel32(X64Buf *buf, uint32_t patch_pos, uint32_t target_offset) {
    /* rel32 displacement is relative to the end of the instruction,
     * which is patch_pos + 4 (the 4-byte displacement field itself). */
    int32_t rel = (int32_t)(target_offset - (patch_pos + 4));
    memcpy(&buf->code[patch_pos], &rel, 4);
}

#endif // XIR_X64_H
