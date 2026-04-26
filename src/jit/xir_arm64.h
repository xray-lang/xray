/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_arm64.h - ARM64 instruction encoding for JIT code emission
 *
 * KEY CONCEPT:
 *   Minimal ARM64 assembler: encode individual instructions to uint32_t.
 *   All functions are pure (no state), emit is done by writing to a buffer.
 *
 * WHY THIS DESIGN:
 *   - Each function encodes one ARM64 instruction as a uint32_t
 *   - Caller writes to code buffer (from xir_code_alloc)
 *   - Keeps encoder stateless and testable
 *   - Covers the subset needed for Tier 1 JIT (i64 arith + control flow)
 */

#ifndef XIR_ARM64_H
#define XIR_ARM64_H

#include <stdint.h>
#include <stdbool.h>
#include "../base/xdefs.h"

/* ========== ARM64 Register Names ========== */

typedef enum {
    // General purpose registers (64-bit X, 32-bit W)
    A64_X0 = 0,
    A64_X1,
    A64_X2,
    A64_X3,
    A64_X4,
    A64_X5,
    A64_X6,
    A64_X7,
    A64_X8,
    A64_X9,
    A64_X10,
    A64_X11,
    A64_X12,
    A64_X13,
    A64_X14,
    A64_X15,
    A64_X16,
    A64_X17,
    A64_X18,
    A64_X19,
    A64_X20,
    A64_X21,
    A64_X22,
    A64_X23,
    A64_X24,
    A64_X25,
    A64_X26,
    A64_X27,
    A64_X28,
    A64_FP = 29,   // frame pointer (x29)
    A64_LR = 30,   // link register (x30)
    A64_SP = 31,   // stack pointer (encoded as 31 in some contexts)
    A64_XZR = 31,  // zero register (same encoding as SP, context-dependent)
} A64Reg;

// Callee-saved: x19-x28, fp(x29), lr(x30)
// Caller-saved: x0-x18
// Scratch: x16, x17 (IP0, IP1)

// Allocatable registers for JIT (avoid x16-x18 scratch, fp, lr, sp)
#define A64_ALLOC_REGS_COUNT 19  // x0-x15, x19-x21

/* ========== ARM64 Condition Codes ========== */

typedef enum {
    A64_CC_EQ = 0,   // equal (Z=1)
    A64_CC_NE = 1,   // not equal (Z=0)
    A64_CC_CS = 2,   // carry set / unsigned >=
    A64_CC_CC = 3,   // carry clear / unsigned <
    A64_CC_MI = 4,   // negative
    A64_CC_PL = 5,   // positive or zero
    A64_CC_VS = 6,   // overflow
    A64_CC_VC = 7,   // no overflow
    A64_CC_HI = 8,   // unsigned >
    A64_CC_LS = 9,   // unsigned <=
    A64_CC_GE = 10,  // signed >=
    A64_CC_LT = 11,  // signed <
    A64_CC_GT = 12,  // signed >
    A64_CC_LE = 13,  // signed <=
    A64_CC_AL = 14,  // always
} A64Cond;

/* ========== Code Buffer ========== */

typedef struct {
    uint32_t *code;     // instruction buffer
    uint32_t count;     // instructions written
    uint32_t capacity;  // buffer capacity (in instructions)
} A64Buf;

XR_FUNC void a64_buf_init(A64Buf *buf, uint32_t *mem, uint32_t cap_instructions);
XR_FUNC void a64_buf_emit(A64Buf *buf, uint32_t inst);
XR_FUNC uint32_t a64_buf_offset(A64Buf *buf);  // current offset in bytes

/* ========== Arithmetic (64-bit) ========== */

// ADD Xd, Xn, Xm
XR_FUNC uint32_t a64_add(A64Reg rd, A64Reg rn, A64Reg rm);

// ADD Xd, Xn, #imm12
XR_FUNC uint32_t a64_add_imm(A64Reg rd, A64Reg rn, uint32_t imm12);

// ADD Xd, Xn, Xm, LSL #shift  (shift: 0-63)
XR_FUNC uint32_t a64_add_lsl(A64Reg rd, A64Reg rn, A64Reg rm, uint32_t shift);

// SUB Xd, Xn, Xm
XR_FUNC uint32_t a64_sub(A64Reg rd, A64Reg rn, A64Reg rm);

// SUB Xd, Xn, #imm12
XR_FUNC uint32_t a64_sub_imm(A64Reg rd, A64Reg rn, uint32_t imm12);

// SUBS Xd, Xn, Xm (sets flags)
XR_FUNC uint32_t a64_subs(A64Reg rd, A64Reg rn, A64Reg rm);

// MUL Xd, Xn, Xm
XR_FUNC uint32_t a64_mul(A64Reg rd, A64Reg rn, A64Reg rm);

// SDIV Xd, Xn, Xm (signed divide)
XR_FUNC uint32_t a64_sdiv(A64Reg rd, A64Reg rn, A64Reg rm);

// MSUB Xd, Xn, Xm, Xa  (Xd = Xa - Xn * Xm)
XR_FUNC uint32_t a64_msub(A64Reg rd, A64Reg rn, A64Reg rm, A64Reg ra);

// NEG Xd, Xm  (alias: SUB Xd, XZR, Xm)
XR_FUNC uint32_t a64_neg(A64Reg rd, A64Reg rm);

/* ========== Logical ========== */

// AND Xd, Xn, Xm
XR_FUNC uint32_t a64_and(A64Reg rd, A64Reg rn, A64Reg rm);

// ORR Xd, Xn, Xm
XR_FUNC uint32_t a64_orr(A64Reg rd, A64Reg rn, A64Reg rm);

// EOR Xd, Xn, Xm
XR_FUNC uint32_t a64_eor(A64Reg rd, A64Reg rn, A64Reg rm);

// MVN Xd, Xm  (alias: ORN Xd, XZR, Xm)
XR_FUNC uint32_t a64_mvn(A64Reg rd, A64Reg rm);

// LSL Xd, Xn, Xm (alias: LSLV)
XR_FUNC uint32_t a64_lsl(A64Reg rd, A64Reg rn, A64Reg rm);

// ASR Xd, Xn, Xm (alias: ASRV)
XR_FUNC uint32_t a64_asr(A64Reg rd, A64Reg rn, A64Reg rm);

// LSR Wd, Wn, #shift (32-bit logical shift right by immediate, alias: UBFM)
XR_FUNC uint32_t a64_lsr_imm(A64Reg rd, A64Reg rn, uint32_t shift);

// LSR Xd, Xn, #shift (64-bit logical shift right by immediate)
XR_FUNC uint32_t a64_lsr_imm64(A64Reg rd, A64Reg rn, uint32_t shift);

// UBFX Xd, Xn, #lsb, #width (64-bit unsigned bitfield extract)
XR_FUNC uint32_t a64_ubfx64(A64Reg rd, A64Reg rn, uint32_t lsb, uint32_t width);

/* ========== Compare ========== */

// CMP Xn, Xm  (alias: SUBS XZR, Xn, Xm)
XR_FUNC uint32_t a64_cmp(A64Reg rn, A64Reg rm);

// CMP Xn, #imm12
XR_FUNC uint32_t a64_cmp_imm(A64Reg rn, uint32_t imm12);

// TST Xn, #bitmask_imm (alias: ANDS XZR, Xn, #imm — sets NZCV flags)
// bitmask_imm must be a valid ARM64 logical immediate (e.g. 0x7, 0xFF, powers-of-2 - 1)
XR_FUNC uint32_t a64_tst_imm(A64Reg rn, uint64_t bitmask_imm);

// CSET Xd, cond (set to 1 if condition, 0 otherwise)
XR_FUNC uint32_t a64_cset(A64Reg rd, A64Cond cond);

// CSEL Xd, Xn, Xm, cond (conditional select: Xd = cond ? Xn : Xm)
XR_FUNC uint32_t a64_csel(A64Reg rd, A64Reg rn, A64Reg rm, A64Cond cond);

// FCSEL Dd, Dn, Dm, cond (FP conditional select: Dd = cond ? Dn : Dm)
XR_FUNC uint32_t a64_fcsel(A64Reg rd, A64Reg rn, A64Reg rm, A64Cond cond);

/* ========== Move / Constants ========== */

// MOV Xd, Xn (alias: ORR Xd, XZR, Xn)
XR_FUNC uint32_t a64_mov(A64Reg rd, A64Reg rn);

// MOVZ Xd, #imm16, LSL #shift (shift: 0, 16, 32, 48)
XR_FUNC uint32_t a64_movz(A64Reg rd, uint16_t imm16, uint8_t shift);

// MOVK Xd, #imm16, LSL #shift (keep other bits)
XR_FUNC uint32_t a64_movk(A64Reg rd, uint16_t imm16, uint8_t shift);

/* ========== Branch ========== */

// B offset (unconditional, PC-relative, offset in instructions)
XR_FUNC uint32_t a64_b(int32_t offset_insts);

// B.cond offset (conditional)
XR_FUNC uint32_t a64_bcond(A64Cond cond, int32_t offset_insts);

// BL offset (branch with link, call)
XR_FUNC uint32_t a64_bl(int32_t offset_insts);

// BLR Xn (branch with link to register)
XR_FUNC uint32_t a64_blr(A64Reg rn);

// BR Xn (branch to register)
XR_FUNC uint32_t a64_br(A64Reg rn);

// RET (alias: BR X30)
XR_FUNC uint32_t a64_ret(void);

// CBZ Xt, offset (compare and branch if zero)
XR_FUNC uint32_t a64_cbz(A64Reg rt, int32_t offset_insts);

// CBNZ Xt, offset (compare and branch if not zero)
XR_FUNC uint32_t a64_cbnz(A64Reg rt, int32_t offset_insts);

/* ========== Memory ========== */

// LDR Xt, [Xn, #offset] (unsigned offset, scaled by 8)
XR_FUNC uint32_t a64_ldr(A64Reg rt, A64Reg rn, int32_t offset);

// STR Xt, [Xn, #offset]
XR_FUNC uint32_t a64_str(A64Reg rt, A64Reg rn, int32_t offset);

// LDP Xt1, Xt2, [Xn, #offset] (load pair)
XR_FUNC uint32_t a64_ldp(A64Reg rt1, A64Reg rt2, A64Reg rn, int32_t offset);

// STP Xt1, Xt2, [Xn, #offset] (store pair)
XR_FUNC uint32_t a64_stp(A64Reg rt1, A64Reg rt2, A64Reg rn, int32_t offset);

// STP pre-index: STP Xt1, Xt2, [Xn, #offset]!
XR_FUNC uint32_t a64_stp_pre(A64Reg rt1, A64Reg rt2, A64Reg rn, int32_t offset);

// LDP post-index: LDP Xt1, Xt2, [Xn], #offset
XR_FUNC uint32_t a64_ldp_post(A64Reg rt1, A64Reg rt2, A64Reg rn, int32_t offset);

/* ========== 8-bit Memory ========== */

// LDRB Wt, [Xn, #offset] (8-bit unsigned byte load, unscaled offset)
XR_FUNC uint32_t a64_ldrb(A64Reg rt, A64Reg rn, int32_t offset);

// STRB Wt, [Xn, #offset] (8-bit byte store, unscaled offset)
XR_FUNC uint32_t a64_strb(A64Reg rt, A64Reg rn, int32_t offset);

/* ========== 16-bit Memory (for shape_id, etc.) ========== */

// LDRH Wt, [Xn, #offset] (16-bit unsigned load, unsigned offset scaled by 2)
XR_FUNC uint32_t a64_ldrh(A64Reg rt, A64Reg rn, int32_t offset);

// STRH Wt, [Xn, #offset] (16-bit store, unsigned offset scaled by 2)
XR_FUNC uint32_t a64_strh(A64Reg rt, A64Reg rn, int32_t offset);

/* ========== 32-bit Memory (for int32 fields like reductions) ========== */

// LDR Wt, [Xn, #offset] (32-bit load, unsigned offset scaled by 4)
XR_FUNC uint32_t a64_ldr_w(A64Reg rt, A64Reg rn, int32_t offset);

// STR Wt, [Xn, #offset] (32-bit store)
XR_FUNC uint32_t a64_str_w(A64Reg rt, A64Reg rn, int32_t offset);

// SUBS Wd, Wn, #imm (32-bit subtract with flags)
XR_FUNC uint32_t a64_subs_imm_w(A64Reg rd, A64Reg rn, uint32_t imm12);

/* ========== Conditional Branch ========== */

// B.cond offset (conditional branch, offset in instructions)
XR_FUNC uint32_t a64_b_cond(A64Cond cond, int32_t offset_insts);

/* ========== Helper: Load 64-bit Immediate ========== */

// Load a full 64-bit immediate into Xd (up to 4 instructions)
// Returns number of instructions emitted
XR_FUNC int a64_load_imm64(A64Buf *buf, A64Reg rd, uint64_t imm);

/* ========== Floating-Point Arithmetic (double, 64-bit) ========== */

// FADD Dd, Dn, Dm
XR_FUNC uint32_t a64_fadd(A64Reg rd, A64Reg rn, A64Reg rm);

// FSUB Dd, Dn, Dm
XR_FUNC uint32_t a64_fsub(A64Reg rd, A64Reg rn, A64Reg rm);

// FMUL Dd, Dn, Dm
XR_FUNC uint32_t a64_fmul(A64Reg rd, A64Reg rn, A64Reg rm);

// FDIV Dd, Dn, Dm
XR_FUNC uint32_t a64_fdiv(A64Reg rd, A64Reg rn, A64Reg rm);

// FNEG Dd, Dn
XR_FUNC uint32_t a64_fneg(A64Reg rd, A64Reg rn);

// FMOV Dd, Dn (FPR to FPR)
XR_FUNC uint32_t a64_fmov(A64Reg rd, A64Reg rn);

// FMOV Dd, Xn (GP to FPR, bit-level transfer)
XR_FUNC uint32_t a64_fmov_gp_to_fp(A64Reg dd, A64Reg xn);

// FCMP Dn, Dm (sets NZCV flags)
XR_FUNC uint32_t a64_fcmp(A64Reg rn, A64Reg rm);

/* ========== Floating-Point Conversion ========== */

// SCVTF Dd, Xn (signed int64 → double)
XR_FUNC uint32_t a64_scvtf(A64Reg rd, A64Reg rn);

// FCVTZS Xd, Dn (double → signed int64, truncate toward zero)
XR_FUNC uint32_t a64_fcvtzs(A64Reg rd, A64Reg rn);

/* ========== Floating-Point Move (GPR ↔ FPR) ========== */

// FMOV Dd, Xn (GPR → FPR, bit-for-bit)
XR_FUNC uint32_t a64_fmov_from_gpr(A64Reg dd, A64Reg xn);

// FMOV Xd, Dn (FPR → GPR, bit-for-bit)
XR_FUNC uint32_t a64_fmov_to_gpr(A64Reg xd, A64Reg dn);

/* ========== Floating-Point Memory ========== */

// LDR Dt, [Xn, #offset] (64-bit FP load, unsigned offset scaled by 8)
XR_FUNC uint32_t a64_ldr_fp(A64Reg rt, A64Reg rn, int32_t offset);

// STR Dt, [Xn, #offset] (64-bit FP store)
XR_FUNC uint32_t a64_str_fp(A64Reg rt, A64Reg rn, int32_t offset);

// STP Dt1, Dt2, [Xn, #offset] (64-bit FP pair store, signed offset scaled by 8)
XR_FUNC uint32_t a64_stp_fp(A64Reg rt1, A64Reg rt2, A64Reg rn, int32_t offset);

// LDP Dt1, Dt2, [Xn, #offset] (64-bit FP pair load, signed offset scaled by 8)
XR_FUNC uint32_t a64_ldp_fp(A64Reg rt1, A64Reg rt2, A64Reg rn, int32_t offset);

/* ========== Helper: Load f64 Immediate ========== */

// Load a double via GPR intermediate (MOVZ/MOVK → FMOV)
// Returns number of instructions emitted
XR_FUNC int a64_load_f64(A64Buf *buf, A64Reg dd, A64Reg scratch_gpr, double val);

/* ========== NOP ========== */

XR_FUNC uint32_t a64_nop(void);

#endif  // XIR_ARM64_H
