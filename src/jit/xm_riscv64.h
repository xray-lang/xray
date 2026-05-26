/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xm_riscv64.h - RISC-V 64 instruction encoding for JIT code emission
 *
 * KEY CONCEPT:
 *   Minimal RV64GD assembler: each function returns a 32-bit instruction word.
 *   Caller writes to a code buffer via rv64_buf_emit().
 *   Encoding functions are generated from xisa/arch/riscv64.isa by xisagen.
 *
 * WHY THIS DESIGN:
 *   - Fixed-width 32-bit instructions: each encoder is a pure function
 *   - Stateless and independently testable (golden-bytes CI)
 *   - Covers RV64I base + M (mul/div) + D (double-precision FP)
 *
 * CALLING CONVENTION (LP64D — standard RISC-V Linux):
 *   Args:     a0-a7  (x10-x17) integer, fa0-fa7 (f10-f17) float
 *   Return:   a0/a1  (x10/x11) integer, fa0/fa1 (f10/f11) float
 *   Caller-saved: t0-t6 (x5-x7,x28-x31), a0-a7, ft0-ft11, fa0-fa7
 *   Callee-saved: s0-s11 (x8-x9,x18-x27), fs0-fs11 (f8-f9,f18-f27)
 *   sp=x2, gp=x3, tp=x4 (reserved), ra=x1 (link register)
 */

#ifndef XM_RISCV64_H
#define XM_RISCV64_H

#include <stdint.h>
#include <stdbool.h>
#include "../base/xdefs.h"
#include "../base/xchecks.h"

/* ========== RISC-V 64 Integer Register Names ========== */

typedef enum {
    RV64_X0 = 0,   /* zero — hardwired zero */
    RV64_RA = 1,   /* x1  — return address */
    RV64_SP = 2,   /* x2  — stack pointer */
    RV64_GP = 3,   /* x3  — global pointer (reserved) */
    RV64_TP = 4,   /* x4  — thread pointer (reserved) */
    RV64_T0 = 5,   /* x5  — temp / alternate link */
    RV64_T1 = 6,   /* x6  — temp */
    RV64_T2 = 7,   /* x7  — temp */
    RV64_S0 = 8,   /* x8  — saved / frame pointer */
    RV64_FP = 8,   /* x8  — frame pointer alias */
    RV64_S1 = 9,   /* x9  — saved */
    RV64_A0 = 10,  /* x10 — arg0 / return value */
    RV64_A1 = 11,  /* x11 — arg1 / return value */
    RV64_A2 = 12,  /* x12 — arg2 */
    RV64_A3 = 13,  /* x13 — arg3 */
    RV64_A4 = 14,  /* x14 — arg4 */
    RV64_A5 = 15,  /* x15 — arg5 */
    RV64_A6 = 16,  /* x16 — arg6 */
    RV64_A7 = 17,  /* x17 — arg7 */
    RV64_S2 = 18,  /* x18 — saved */
    RV64_S3 = 19,  /* x19 — saved */
    RV64_S4 = 20,  /* x20 — saved */
    RV64_S5 = 21,  /* x21 — saved */
    RV64_S6 = 22,  /* x22 — saved */
    RV64_S7 = 23,  /* x23 — saved */
    RV64_S8 = 24,  /* x24 — saved */
    RV64_S9 = 25,  /* x25 — saved */
    RV64_S10 = 26, /* x26 — saved */
    RV64_S11 = 27, /* x27 — saved */
    RV64_T3 = 28,  /* x28 — temp */
    RV64_T4 = 29,  /* x29 — temp */
    RV64_T5 = 30,  /* x30 — temp */
    RV64_T6 = 31,  /* x31 — temp */
} Rv64Reg;

/* ========== RISC-V 64 FP Register Names (D extension) ========== */

typedef enum {
    RV64_FT0 = 0,   /* f0  — FP temp */
    RV64_FT1 = 1,   /* f1  — FP temp */
    RV64_FT2 = 2,   /* f2  — FP temp */
    RV64_FT3 = 3,   /* f3  — FP temp */
    RV64_FT4 = 4,   /* f4  — FP temp */
    RV64_FT5 = 5,   /* f5  — FP temp */
    RV64_FT6 = 6,   /* f6  — FP temp */
    RV64_FT7 = 7,   /* f7  — FP temp */
    RV64_FS0 = 8,   /* f8  — FP saved */
    RV64_FS1 = 9,   /* f9  — FP saved */
    RV64_FA0 = 10,  /* f10 — FP arg0 / return */
    RV64_FA1 = 11,  /* f11 — FP arg1 / return */
    RV64_FA2 = 12,  /* f12 — FP arg2 */
    RV64_FA3 = 13,  /* f13 — FP arg3 */
    RV64_FA4 = 14,  /* f14 — FP arg4 */
    RV64_FA5 = 15,  /* f15 — FP arg5 */
    RV64_FA6 = 16,  /* f16 — FP arg6 */
    RV64_FA7 = 17,  /* f17 — FP arg7 */
    RV64_FS2 = 18,  /* f18 — FP saved */
    RV64_FS3 = 19,  /* f19 — FP saved */
    RV64_FS4 = 20,  /* f20 — FP saved */
    RV64_FS5 = 21,  /* f21 — FP saved */
    RV64_FS6 = 22,  /* f22 — FP saved */
    RV64_FS7 = 23,  /* f23 — FP saved */
    RV64_FS8 = 24,  /* f24 — FP saved */
    RV64_FS9 = 25,  /* f25 — FP saved */
    RV64_FS10 = 26, /* f26 — FP saved */
    RV64_FS11 = 27, /* f27 — FP saved */
    RV64_FT8 = 28,  /* f28 — FP temp */
    RV64_FT9 = 29,  /* f29 — FP temp */
    RV64_FT10 = 30, /* f30 — FP temp */
    RV64_FT11 = 31, /* f31 — FP temp */
} Rv64Freg;

/* ========== RISC-V Branch Condition Codes ========== */

/* Encoded as funct3 field of B-type instructions.
 * Used by the generic comparison + conditional-branch pattern. */
typedef enum {
    RV64_CC_EQ = 0,  /* beq:  rs1 == rs2 */
    RV64_CC_NE = 1,  /* bne:  rs1 != rs2 */
    RV64_CC_LT = 4,  /* blt:  rs1 <  rs2 (signed) */
    RV64_CC_GE = 5,  /* bge:  rs1 >= rs2 (signed) */
    RV64_CC_LTU = 6, /* bltu: rs1 <  rs2 (unsigned) */
    RV64_CC_GEU = 7, /* bgeu: rs1 >= rs2 (unsigned) */
} Rv64Cond;

/* ========== JIT-Reserved Registers ========== */

/* JIT register assignment for Xray RISC-V backend:
 *   s11 (x27) = coroutine pointer  (callee-saved, reserved)
 *   s10 (x26) = jit_ctx pointer    (callee-saved, reserved)
 *   s0  (x8)  = frame pointer      (callee-saved, reserved)
 *   sp  (x2)  = stack pointer      (reserved)
 *   t6  (x31) = scratch register   (caller-saved, not allocatable)
 *   t5  (x30) = scratch register 2 (caller-saved, not allocatable)
 */
#define RV64_SCRATCH_REG RV64_T6
#define RV64_SCRATCH_REG2 RV64_T5
#define RV64_CORO_REG RV64_S11
#define RV64_JIT_CTX_REG RV64_S10

/* ========== Code Buffer ========== */

/* Fixed-width 32-bit instruction buffer (same model as ARM64). */
typedef struct {
    uint32_t *code;    /* instruction buffer */
    uint32_t count;    /* instructions written */
    uint32_t capacity; /* buffer capacity (in instructions) */
} Rv64Buf;

static inline void rv64_buf_init(Rv64Buf *buf, uint32_t *mem, uint32_t cap_instructions) {
    XR_DCHECK(buf != NULL, "rv64_buf_init: NULL buf");
    XR_DCHECK(mem != NULL, "rv64_buf_init: NULL mem");
    buf->code = mem;
    buf->count = 0;
    buf->capacity = cap_instructions;
}

static inline void rv64_buf_emit(Rv64Buf *buf, uint32_t inst) {
    XR_DCHECK(buf != NULL, "rv64_buf_emit: NULL buf");
    XR_DCHECK(buf->count < buf->capacity, "rv64_buf_emit: buffer overflow");
    buf->code[buf->count++] = inst;
}

static inline uint32_t rv64_buf_offset(Rv64Buf *buf) {
    return buf->count * 4;
}

/* ========== Encoding Functions (generated from riscv64.isa) ========== */
/* Full declarations: see xm_riscv64_gen.c */

/* --- RV64I: Integer Arithmetic (R-type) --- */
XR_FUNC uint32_t rv64_add(uint8_t rd, uint8_t rs1, uint8_t rs2);
XR_FUNC uint32_t rv64_sub(uint8_t rd, uint8_t rs1, uint8_t rs2);
XR_FUNC uint32_t rv64_and(uint8_t rd, uint8_t rs1, uint8_t rs2);
XR_FUNC uint32_t rv64_or(uint8_t rd, uint8_t rs1, uint8_t rs2);
XR_FUNC uint32_t rv64_xor(uint8_t rd, uint8_t rs1, uint8_t rs2);
XR_FUNC uint32_t rv64_sll(uint8_t rd, uint8_t rs1, uint8_t rs2);
XR_FUNC uint32_t rv64_srl(uint8_t rd, uint8_t rs1, uint8_t rs2);
XR_FUNC uint32_t rv64_sra(uint8_t rd, uint8_t rs1, uint8_t rs2);
XR_FUNC uint32_t rv64_slt(uint8_t rd, uint8_t rs1, uint8_t rs2);
XR_FUNC uint32_t rv64_sltu(uint8_t rd, uint8_t rs1, uint8_t rs2);

/* --- RV64I: Word-width Arithmetic (R-type, *W) --- */
XR_FUNC uint32_t rv64_addw(uint8_t rd, uint8_t rs1, uint8_t rs2);
XR_FUNC uint32_t rv64_subw(uint8_t rd, uint8_t rs1, uint8_t rs2);
XR_FUNC uint32_t rv64_sllw(uint8_t rd, uint8_t rs1, uint8_t rs2);
XR_FUNC uint32_t rv64_srlw(uint8_t rd, uint8_t rs1, uint8_t rs2);
XR_FUNC uint32_t rv64_sraw(uint8_t rd, uint8_t rs1, uint8_t rs2);

/* --- RV64I: Immediate Arithmetic (I-type) --- */
XR_FUNC uint32_t rv64_addi(uint8_t rd, uint8_t rs1, int32_t imm);
XR_FUNC uint32_t rv64_andi(uint8_t rd, uint8_t rs1, int32_t imm);
XR_FUNC uint32_t rv64_ori(uint8_t rd, uint8_t rs1, int32_t imm);
XR_FUNC uint32_t rv64_xori(uint8_t rd, uint8_t rs1, int32_t imm);
XR_FUNC uint32_t rv64_slti(uint8_t rd, uint8_t rs1, int32_t imm);
XR_FUNC uint32_t rv64_sltiu(uint8_t rd, uint8_t rs1, int32_t imm);

/* --- RV64I: Shift Immediate --- */
XR_FUNC uint32_t rv64_slli(uint8_t rd, uint8_t rs1, int32_t shamt);
XR_FUNC uint32_t rv64_srli(uint8_t rd, uint8_t rs1, int32_t shamt);
XR_FUNC uint32_t rv64_srai(uint8_t rd, uint8_t rs1, int32_t shamt);
XR_FUNC uint32_t rv64_addiw(uint8_t rd, uint8_t rs1, int32_t imm);
XR_FUNC uint32_t rv64_slliw(uint8_t rd, uint8_t rs1, int32_t shamt);
XR_FUNC uint32_t rv64_srliw(uint8_t rd, uint8_t rs1, int32_t shamt);
XR_FUNC uint32_t rv64_sraiw(uint8_t rd, uint8_t rs1, int32_t shamt);

/* --- RV64I: Load / Store --- */
XR_FUNC uint32_t rv64_lb(uint8_t rd, uint8_t rs1, int32_t imm);
XR_FUNC uint32_t rv64_lh(uint8_t rd, uint8_t rs1, int32_t imm);
XR_FUNC uint32_t rv64_lw(uint8_t rd, uint8_t rs1, int32_t imm);
XR_FUNC uint32_t rv64_ld(uint8_t rd, uint8_t rs1, int32_t imm);
XR_FUNC uint32_t rv64_lbu(uint8_t rd, uint8_t rs1, int32_t imm);
XR_FUNC uint32_t rv64_lhu(uint8_t rd, uint8_t rs1, int32_t imm);
XR_FUNC uint32_t rv64_lwu(uint8_t rd, uint8_t rs1, int32_t imm);
XR_FUNC uint32_t rv64_sb(uint8_t rs2, uint8_t rs1, int32_t imm);
XR_FUNC uint32_t rv64_sh(uint8_t rs2, uint8_t rs1, int32_t imm);
XR_FUNC uint32_t rv64_sw(uint8_t rs2, uint8_t rs1, int32_t imm);
XR_FUNC uint32_t rv64_sd(uint8_t rs2, uint8_t rs1, int32_t imm);

/* --- RV64I: Control Flow --- */
XR_FUNC uint32_t rv64_jal(uint8_t rd, int32_t imm);
XR_FUNC uint32_t rv64_jalr(uint8_t rd, uint8_t rs1, int32_t imm);
XR_FUNC uint32_t rv64_beq(uint8_t rs1, uint8_t rs2, int32_t imm);
XR_FUNC uint32_t rv64_bne(uint8_t rs1, uint8_t rs2, int32_t imm);
XR_FUNC uint32_t rv64_blt(uint8_t rs1, uint8_t rs2, int32_t imm);
XR_FUNC uint32_t rv64_bge(uint8_t rs1, uint8_t rs2, int32_t imm);
XR_FUNC uint32_t rv64_bltu(uint8_t rs1, uint8_t rs2, int32_t imm);
XR_FUNC uint32_t rv64_bgeu(uint8_t rs1, uint8_t rs2, int32_t imm);

/* --- RV64I: Upper Immediate --- */
XR_FUNC uint32_t rv64_lui(uint8_t rd, uint32_t imm);
XR_FUNC uint32_t rv64_auipc(uint8_t rd, uint32_t imm);

/* --- RV64I: Miscellaneous --- */
XR_FUNC uint32_t rv64_nop(void);
XR_FUNC uint32_t rv64_ebreak(void);

/* --- RV64M: Multiply / Divide --- */
XR_FUNC uint32_t rv64_mul(uint8_t rd, uint8_t rs1, uint8_t rs2);
XR_FUNC uint32_t rv64_mulh(uint8_t rd, uint8_t rs1, uint8_t rs2);
XR_FUNC uint32_t rv64_mulhu(uint8_t rd, uint8_t rs1, uint8_t rs2);
XR_FUNC uint32_t rv64_div(uint8_t rd, uint8_t rs1, uint8_t rs2);
XR_FUNC uint32_t rv64_divu(uint8_t rd, uint8_t rs1, uint8_t rs2);
XR_FUNC uint32_t rv64_rem(uint8_t rd, uint8_t rs1, uint8_t rs2);
XR_FUNC uint32_t rv64_remu(uint8_t rd, uint8_t rs1, uint8_t rs2);
XR_FUNC uint32_t rv64_mulw(uint8_t rd, uint8_t rs1, uint8_t rs2);
XR_FUNC uint32_t rv64_divw(uint8_t rd, uint8_t rs1, uint8_t rs2);
XR_FUNC uint32_t rv64_divuw(uint8_t rd, uint8_t rs1, uint8_t rs2);
XR_FUNC uint32_t rv64_remw(uint8_t rd, uint8_t rs1, uint8_t rs2);
XR_FUNC uint32_t rv64_remuw(uint8_t rd, uint8_t rs1, uint8_t rs2);

/* --- RV64D: Double-precision FP --- */
XR_FUNC uint32_t rv64_fld(uint8_t rd, uint8_t rs1, int32_t imm);
XR_FUNC uint32_t rv64_fsd(uint8_t rs2, uint8_t rs1, int32_t imm);
XR_FUNC uint32_t rv64_fadd_d(uint8_t rd, uint8_t rs1, uint8_t rs2);
XR_FUNC uint32_t rv64_fsub_d(uint8_t rd, uint8_t rs1, uint8_t rs2);
XR_FUNC uint32_t rv64_fmul_d(uint8_t rd, uint8_t rs1, uint8_t rs2);
XR_FUNC uint32_t rv64_fdiv_d(uint8_t rd, uint8_t rs1, uint8_t rs2);
XR_FUNC uint32_t rv64_fsqrt_d(uint8_t rd, uint8_t rs1);
XR_FUNC uint32_t rv64_feq_d(uint8_t rd, uint8_t rs1, uint8_t rs2);
XR_FUNC uint32_t rv64_flt_d(uint8_t rd, uint8_t rs1, uint8_t rs2);
XR_FUNC uint32_t rv64_fle_d(uint8_t rd, uint8_t rs1, uint8_t rs2);
XR_FUNC uint32_t rv64_fcvt_d_l(uint8_t rd, uint8_t rs1);
XR_FUNC uint32_t rv64_fcvt_l_d(uint8_t rd, uint8_t rs1);
XR_FUNC uint32_t rv64_fmv_x_d(uint8_t rd, uint8_t rs1);
XR_FUNC uint32_t rv64_fmv_d_x(uint8_t rd, uint8_t rs1);

/* ========== Pseudo-instruction Helpers ========== */

/* MV rd, rs  → addi rd, rs, 0 */
static inline uint32_t rv64_mv(uint8_t rd, uint8_t rs) {
    return rv64_addi(rd, rs, 0);
}

/* LI rd, imm (load 12-bit immediate) → addi rd, x0, imm */
static inline uint32_t rv64_li(uint8_t rd, int32_t imm) {
    return rv64_addi(rd, RV64_X0, imm);
}

/* J offset → jal x0, offset */
static inline uint32_t rv64_j(int32_t offset) {
    return rv64_jal(RV64_X0, offset);
}

/* CALL offset → jal ra, offset */
static inline uint32_t rv64_call(int32_t offset) {
    return rv64_jal(RV64_RA, offset);
}

/* RET → jalr x0, ra, 0 */
static inline uint32_t rv64_ret(void) {
    return rv64_jalr(RV64_X0, RV64_RA, 0);
}

/* NEG rd, rs → sub rd, x0, rs */
static inline uint32_t rv64_neg(uint8_t rd, uint8_t rs) {
    return rv64_sub(rd, RV64_X0, rs);
}

/* NOT rd, rs → xori rd, rs, -1 */
static inline uint32_t rv64_not(uint8_t rd, uint8_t rs) {
    return rv64_xori(rd, rs, -1);
}

/* SEQZ rd, rs → sltiu rd, rs, 1 */
static inline uint32_t rv64_seqz(uint8_t rd, uint8_t rs) {
    return rv64_sltiu(rd, rs, 1);
}

/* SNEZ rd, rs → sltu rd, x0, rs */
static inline uint32_t rv64_snez(uint8_t rd, uint8_t rs) {
    return rv64_sltu(rd, RV64_X0, rs);
}

/* FNEG.D rd, rs → fsgnjn.d rd, rs, rs (sign-inject negate)
 * Encoding: funct7=0x11 (0b0010001), funct3=1, opcode=0x53 */
static inline uint32_t rv64_fneg_d(uint8_t rd, uint8_t rs) {
    return 0x53u | ((uint32_t) rd << 7) | (0x1u << 12) | ((uint32_t) rs << 15) |
           ((uint32_t) rs << 20) | (0x11u << 25);
}

#endif  // XM_RISCV64_H
