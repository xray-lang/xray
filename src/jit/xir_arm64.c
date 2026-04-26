/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_arm64.c - ARM64 instruction encoding for JIT code emission
 *
 * KEY CONCEPT:
 *   Pure encoding functions: each returns a uint32_t ARM64 instruction.
 *   No state, no side effects. Caller writes to code buffer.
 *
 * RELATED MODULES:
 *   - xir_code_alloc.h: executable memory for the emitted code
 *   - xir_arm64.h: public API and register definitions
 */

#ifdef __aarch64__

#include "xir_arm64.h"
#include "../base/xchecks.h"
#include <string.h>

/* ========== Code Buffer ========== */

void a64_buf_init(A64Buf *buf, uint32_t *mem, uint32_t cap_instructions) {
    buf->code = mem;
    buf->count = 0;
    buf->capacity = cap_instructions;
}

void a64_buf_emit(A64Buf *buf, uint32_t inst) {
    XR_DCHECK(buf->count < buf->capacity, "assertion failed");
    buf->code[buf->count++] = inst;
}

uint32_t a64_buf_offset(A64Buf *buf) {
    return buf->count * 4;  // each instruction is 4 bytes
}

/* ========== Encoding Helpers ========== */

// Data Processing (register) - 3-source format
//   sf=1 (64-bit), opc, shift=0, Rm, imm6=0, Rn, Rd
static inline uint32_t dp_reg(uint32_t sf, uint32_t opc_top, uint32_t opc_lo, A64Reg rd, A64Reg rn,
                              A64Reg rm) {
    (void) opc_lo;
    // ADD/SUB shifted register: sf|op|S|01011|shift|0|Rm|imm6|Rn|Rd
    return (sf << 31) | (opc_top << 29) | (0x0B << 24) | (0 << 22) | (0 << 21) |
           ((uint32_t) rm << 16) | (0 << 10) | ((uint32_t) rn << 5) | (uint32_t) rd;
}

// Data Processing (immediate) - add/sub
//   sf|op|S|100010|sh|imm12|Rn|Rd
static inline uint32_t dp_imm(uint32_t sf, uint32_t op, uint32_t S, A64Reg rd, A64Reg rn,
                              uint32_t imm12) {
    XR_DCHECK(imm12 < 4096, "assertion failed");
    return (sf << 31) | (op << 30) | (S << 29) | (0x22 << 23) | (0 << 22) | (imm12 << 10) |
           ((uint32_t) rn << 5) | (uint32_t) rd;
}

/* ========== Arithmetic ========== */

uint32_t a64_add(A64Reg rd, A64Reg rn, A64Reg rm) {
    // ADD X (sf=1, op=0, S=0)
    return dp_reg(1, 0, 0, rd, rn, rm);
}

uint32_t a64_add_lsl(A64Reg rd, A64Reg rn, A64Reg rm, uint32_t shift) {
    // ADD Xd, Xn, Xm, LSL #shift: sf|0|0|01011|00|0|Rm|imm6|Rn|Rd
    XR_DCHECK(shift < 64, "assertion failed");
    return (1u << 31) | (0x0B << 24) | (0 << 22) | (0 << 21) | ((uint32_t) rm << 16) |
           (shift << 10) | ((uint32_t) rn << 5) | (uint32_t) rd;
}

uint32_t a64_add_imm(A64Reg rd, A64Reg rn, uint32_t imm12) {
    // ADD X #imm (sf=1, op=0, S=0)
    return dp_imm(1, 0, 0, rd, rn, imm12);
}

uint32_t a64_sub(A64Reg rd, A64Reg rn, A64Reg rm) {
    // SUB X (sf=1, op=1, S=0)
    return dp_reg(1, 2, 0, rd, rn, rm);
}

uint32_t a64_sub_imm(A64Reg rd, A64Reg rn, uint32_t imm12) {
    // SUB X #imm (sf=1, op=1, S=0)
    return dp_imm(1, 1, 0, rd, rn, imm12);
}

uint32_t a64_subs(A64Reg rd, A64Reg rn, A64Reg rm) {
    // SUBS X (sf=1, op=1, S=1)
    return dp_reg(1, 3, 0, rd, rn, rm);
}

uint32_t a64_mul(A64Reg rd, A64Reg rn, A64Reg rm) {
    // MADD Xd, Xn, Xm, XZR (= MUL)
    // 1|00|11011|000|Rm|0|Ra(=11111)|Rn|Rd
    return (1u << 31) | (0x0 << 29) | (0x1B << 24) | (0x0 << 21) | ((uint32_t) rm << 16) |
           (0 << 15) | (0x1F << 10) | ((uint32_t) rn << 5) | (uint32_t) rd;
}

uint32_t a64_sdiv(A64Reg rd, A64Reg rn, A64Reg rm) {
    // SDIV Xd, Xn, Xm
    // 1|00|11010110|Rm|00001|1|Rn|Rd
    return (1u << 31) | (0x0 << 29) | (0xD6 << 21) | ((uint32_t) rm << 16) | (0x03 << 10) |
           ((uint32_t) rn << 5) | (uint32_t) rd;
}

uint32_t a64_msub(A64Reg rd, A64Reg rn, A64Reg rm, A64Reg ra) {
    // MSUB Xd, Xn, Xm, Xa: sf=1|00|11011|000|Rm|1|Ra|Rn|Rd
    return (1u << 31) | (0x0 << 29) | (0x1B << 24) | (0x0 << 21) | ((uint32_t) rm << 16) |
           (1u << 15) | ((uint32_t) ra << 10) | ((uint32_t) rn << 5) | (uint32_t) rd;
}

uint32_t a64_neg(A64Reg rd, A64Reg rm) {
    return a64_sub(rd, A64_XZR, rm);
}

/* ========== Logical ========== */

// Logical shifted register: sf|opc|01010|shift|0|Rm|imm6|Rn|Rd
static inline uint32_t log_reg(uint32_t opc, A64Reg rd, A64Reg rn, A64Reg rm, bool invert) {
    uint32_t N = invert ? 1 : 0;
    return (1u << 31) | (opc << 29) | (0x0A << 24) | (0 << 22) | (N << 21) | ((uint32_t) rm << 16) |
           (0 << 10) | ((uint32_t) rn << 5) | (uint32_t) rd;
}

uint32_t a64_and(A64Reg rd, A64Reg rn, A64Reg rm) {
    return log_reg(0, rd, rn, rm, false);  // AND: opc=00
}

uint32_t a64_orr(A64Reg rd, A64Reg rn, A64Reg rm) {
    return log_reg(1, rd, rn, rm, false);  // ORR: opc=01
}

uint32_t a64_eor(A64Reg rd, A64Reg rn, A64Reg rm) {
    return log_reg(2, rd, rn, rm, false);  // EOR: opc=10
}

uint32_t a64_mvn(A64Reg rd, A64Reg rm) {
    // ORN Xd, XZR, Xm (invert = true)
    return log_reg(1, rd, A64_XZR, rm, true);
}

uint32_t a64_lsl(A64Reg rd, A64Reg rn, A64Reg rm) {
    // LSLV: 1|00|11010110|Rm|0010|00|Rn|Rd
    return (1u << 31) | (0x0 << 29) | (0xD6 << 21) | ((uint32_t) rm << 16) | (0x08 << 10) |
           ((uint32_t) rn << 5) | (uint32_t) rd;
}

uint32_t a64_asr(A64Reg rd, A64Reg rn, A64Reg rm) {
    // ASRV: 1|00|11010110|Rm|0010|10|Rn|Rd
    return (1u << 31) | (0x0 << 29) | (0xD6 << 21) | ((uint32_t) rm << 16) | (0x0A << 10) |
           ((uint32_t) rn << 5) | (uint32_t) rd;
}

uint32_t a64_lsr_imm(A64Reg rd, A64Reg rn, uint32_t shift) {
    // LSR Wd, Wn, #shift  →  UBFM Wd, Wn, #shift, #31
    // Encoding: 0|10|100110|0|immr:6|imms:6|Rn:5|Rd:5
    // sf=0 (32-bit), opc=10, N=0, immr=shift, imms=31
    XR_DCHECK(shift > 0 && shift < 32, "assertion failed");
    return (0x53000000u) | (shift << 16) | (31u << 10) | ((uint32_t) rn << 5) | (uint32_t) rd;
}

uint32_t a64_lsr_imm64(A64Reg rd, A64Reg rn, uint32_t shift) {
    // LSR Xd, Xn, #shift  →  UBFM Xd, Xn, #shift, #63
    // sf=1, opc=10, N=1, immr=shift, imms=63
    XR_DCHECK(shift > 0 && shift < 64, "assertion failed");
    return (0xD3400000u) | (shift << 16) | (63u << 10) | ((uint32_t) rn << 5) | (uint32_t) rd;
}

uint32_t a64_ubfx64(A64Reg rd, A64Reg rn, uint32_t lsb, uint32_t width) {
    // UBFX Xd, Xn, #lsb, #width  →  UBFM Xd, Xn, #lsb, #(lsb+width-1)
    // sf=1, opc=10, N=1, immr=lsb, imms=lsb+width-1
    XR_DCHECK(lsb + width <= 64 && width > 0, "assertion failed");
    uint32_t imms = lsb + width - 1;
    return (0xD3400000u) | (lsb << 16) | (imms << 10) | ((uint32_t) rn << 5) | (uint32_t) rd;
}

/* ========== Compare ========== */

uint32_t a64_cmp(A64Reg rn, A64Reg rm) {
    return a64_subs(A64_XZR, rn, rm);
}

uint32_t a64_cmp_imm(A64Reg rn, uint32_t imm12) {
    // SUBS XZR, Xn, #imm12 (sf=1, op=1, S=1)
    return dp_imm(1, 1, 1, A64_XZR, rn, imm12);
}

uint32_t a64_tst_imm(A64Reg rn, uint64_t bitmask_imm) {
    // TST Xn, #imm = ANDS XZR, Xn, #imm
    // Encoding: sf=1 | opc=11 | 100100 | N | immr | imms | Rn | Rd=11111
    //
    // Supports (2^n - 1) masks: 0x1, 0x3, 0x7, 0xF, 0x1F, 0x3F
    // For 64-bit element (N=1, immr=0), imms = number_of_ones - 1
    uint32_t ones = 0;
    uint64_t v = bitmask_imm;
    XR_DCHECK(v != 0, "bitmask_imm must be non-zero");
    while (v & 1) {
        ones++;
        v >>= 1;
    }
    XR_DCHECK(v == 0 && ones > 0 && ones < 64 &&
                  "bitmask_imm must be (2^n - 1) for this simplified encoder",
              "assertion failed");
    uint32_t N = 1, immr = 0, imms = ones - 1;
    return (1u << 31) | (3u << 29) | (0x24u << 23) | (N << 22) | (immr << 16) | (imms << 10) |
           ((uint32_t) rn << 5) | 0x1F;
}

uint32_t a64_cset(A64Reg rd, A64Cond cond) {
    // CSINC Xd, XZR, XZR, invert(cond)
    // 1|00|11010100|Rm(=11111)|cond_inv|0|0|Rn(=11111)|Rd
    uint32_t cond_inv = cond ^ 1;  // invert condition
    return (1u << 31) | (0x0 << 29) | (0xD4 << 21) | (0x1F << 16) | (cond_inv << 12) | (0x1 << 10) |
           (0x1F << 5) | (uint32_t) rd;
}

uint32_t a64_csel(A64Reg rd, A64Reg rn, A64Reg rm, A64Cond cond) {
    // CSEL Xd, Xn, Xm, cond
    // 1|00|11010100|Rm|cond|0|0|Rn|Rd
    return (1u << 31) | (0x0 << 29) | (0xD4 << 21) | ((uint32_t) rm << 16) |
           ((uint32_t) cond << 12) | (0x0 << 10) | ((uint32_t) rn << 5) | (uint32_t) rd;
}

uint32_t a64_fcsel(A64Reg rd, A64Reg rn, A64Reg rm, A64Cond cond) {
    // FCSEL Dd, Dn, Dm, cond (double precision)
    // 0|00|11110|01|1|Rm|cond|11|Rn|Rd
    return (0x0u << 31) | (0x0 << 29) | (0x1E << 24) | (0x1 << 22) | (1u << 21) |
           ((uint32_t) (rm & 0x1F) << 16) | ((uint32_t) cond << 12) | (0x3 << 10) |
           ((uint32_t) (rn & 0x1F) << 5) | (uint32_t) (rd & 0x1F);
}

/* ========== Move / Constants ========== */

uint32_t a64_mov(A64Reg rd, A64Reg rn) {
    return a64_orr(rd, A64_XZR, rn);
}

uint32_t a64_movz(A64Reg rd, uint16_t imm16, uint8_t shift) {
    // MOVZ: 1|10|100101|hw|imm16|Rd
    uint32_t hw = shift / 16;
    XR_DCHECK(hw <= 3, "assertion failed");
    return (1u << 31) | (0x2 << 29) | (0x25 << 23) | (hw << 21) | ((uint32_t) imm16 << 5) |
           (uint32_t) rd;
}

uint32_t a64_movk(A64Reg rd, uint16_t imm16, uint8_t shift) {
    // MOVK: 1|11|100101|hw|imm16|Rd
    uint32_t hw = shift / 16;
    XR_DCHECK(hw <= 3, "assertion failed");
    return (1u << 31) | (0x3 << 29) | (0x25 << 23) | (hw << 21) | ((uint32_t) imm16 << 5) |
           (uint32_t) rd;
}

/* ========== Branch ========== */

uint32_t a64_b(int32_t offset_insts) {
    // B: 000101|imm26
    uint32_t imm26 = (uint32_t) offset_insts & 0x3FFFFFF;
    return (0x05 << 26) | imm26;
}

uint32_t a64_bcond(A64Cond cond, int32_t offset_insts) {
    // B.cond: 01010100|imm19|0|cond
    uint32_t imm19 = (uint32_t) offset_insts & 0x7FFFF;
    return (0x54 << 24) | (imm19 << 5) | (uint32_t) cond;
}

uint32_t a64_bl(int32_t offset_insts) {
    // BL: 100101|imm26
    uint32_t imm26 = (uint32_t) offset_insts & 0x3FFFFFF;
    return (0x25u << 26) | imm26;
}

uint32_t a64_blr(A64Reg rn) {
    // BLR: 1101011000|1|11111|0000|00|Rn|00000
    return (0xD63Fu << 16) | (0x0u << 10) | ((uint32_t) rn << 5) | 0x0u;
}

uint32_t a64_br(A64Reg rn) {
    // BR: 1101011000|0|11111|0000|00|Rn|00000
    return (0xD61Fu << 16) | (0x0u << 10) | ((uint32_t) rn << 5) | 0x0u;
}

uint32_t a64_ret(void) {
    // RET X30: 1101011001|0|11111|0000|00|11110|00000
    return (0xD65Fu << 16) | (0x0u << 10) | ((uint32_t) A64_LR << 5) | 0x0u;
}

uint32_t a64_cbz(A64Reg rt, int32_t offset_insts) {
    // CBZ: 1|011010|0|imm19|Rt
    uint32_t imm19 = (uint32_t) offset_insts & 0x7FFFF;
    return (1u << 31) | (0x34 << 24) | (imm19 << 5) | (uint32_t) rt;
}

uint32_t a64_cbnz(A64Reg rt, int32_t offset_insts) {
    // CBNZ: 1|011010|1|imm19|Rt
    uint32_t imm19 = (uint32_t) offset_insts & 0x7FFFF;
    return (1u << 31) | (0x35 << 24) | (imm19 << 5) | (uint32_t) rt;
}

/* ========== Memory ========== */

uint32_t a64_ldr(A64Reg rt, A64Reg rn, int32_t offset) {
    // LDR Xt, [Xn, #offset] (unsigned offset, 64-bit)
    // Encoding: 11_111_0_01_01_imm12_Rn_Rt = base 0xF9400000
    XR_DCHECK((offset & 7) == 0 && offset >= 0, "assertion failed");
    uint32_t imm12 = (uint32_t) (offset / 8);
    XR_DCHECK(imm12 < 4096, "assertion failed");
    return 0xF9400000u | (imm12 << 10) | ((uint32_t) rn << 5) | (uint32_t) rt;
}

uint32_t a64_str(A64Reg rt, A64Reg rn, int32_t offset) {
    // STR Xt, [Xn, #offset] (unsigned offset, 64-bit)
    // Encoding: 11_111_0_01_00_imm12_Rn_Rt = base 0xF9000000
    XR_DCHECK((offset & 7) == 0 && offset >= 0, "assertion failed");
    uint32_t imm12 = (uint32_t) (offset / 8);
    XR_DCHECK(imm12 < 4096, "assertion failed");
    return 0xF9000000u | (imm12 << 10) | ((uint32_t) rn << 5) | (uint32_t) rt;
}

uint32_t a64_ldp(A64Reg rt1, A64Reg rt2, A64Reg rn, int32_t offset) {
    // LDP Xt1, Xt2, [Xn, #offset] (signed offset, 64-bit)
    // Encoding: 10_101_0_010_1_imm7_Rt2_Rn_Rt1 = base 0xA9400000
    int32_t imm7 = offset / 8;
    XR_DCHECK(imm7 >= -64 && imm7 < 64, "assertion failed");
    return 0xA9400000u | (((uint32_t) imm7 & 0x7F) << 15) | ((uint32_t) rt2 << 10) |
           ((uint32_t) rn << 5) | (uint32_t) rt1;
}

uint32_t a64_stp(A64Reg rt1, A64Reg rt2, A64Reg rn, int32_t offset) {
    // STP Xt1, Xt2, [Xn, #offset] (signed offset, 64-bit)
    // Encoding: 10_101_0_010_0_imm7_Rt2_Rn_Rt1 = base 0xA9000000
    int32_t imm7 = offset / 8;
    XR_DCHECK(imm7 >= -64 && imm7 < 64, "assertion failed");
    return 0xA9000000u | (((uint32_t) imm7 & 0x7F) << 15) | ((uint32_t) rt2 << 10) |
           ((uint32_t) rn << 5) | (uint32_t) rt1;
}

uint32_t a64_stp_pre(A64Reg rt1, A64Reg rt2, A64Reg rn, int32_t offset) {
    // STP Xt1, Xt2, [Xn, #offset]! (pre-index, 64-bit)
    // Encoding: 10_101_0_011_0_imm7_Rt2_Rn_Rt1 = base 0xA9800000
    int32_t imm7 = offset / 8;
    XR_DCHECK(imm7 >= -64 && imm7 < 64, "assertion failed");
    return 0xA9800000u | (((uint32_t) imm7 & 0x7F) << 15) | ((uint32_t) rt2 << 10) |
           ((uint32_t) rn << 5) | (uint32_t) rt1;
}

uint32_t a64_ldp_post(A64Reg rt1, A64Reg rt2, A64Reg rn, int32_t offset) {
    // LDP Xt1, Xt2, [Xn], #offset (post-index, 64-bit)
    // Encoding: 10_101_0_001_1_imm7_Rt2_Rn_Rt1 = base 0xA8C00000
    int32_t imm7 = offset / 8;
    XR_DCHECK(imm7 >= -64 && imm7 < 64, "assertion failed");
    return 0xA8C00000u | (((uint32_t) imm7 & 0x7F) << 15) | ((uint32_t) rt2 << 10) |
           ((uint32_t) rn << 5) | (uint32_t) rt1;
}

/* ========== 8-bit Memory ========== */

uint32_t a64_ldrb(A64Reg rt, A64Reg rn, int32_t offset) {
    // LDRB Wt, [Xn, #uimm12] (8-bit unsigned byte load, offset NOT scaled)
    // 0011 1001 01 imm12 Rn:5 Rt:5 = base 0x39400000
    XR_DCHECK(offset >= 0 && offset < 4096, "assertion failed");
    uint32_t imm12 = (uint32_t) offset;
    return 0x39400000u | (imm12 << 10) | ((uint32_t) rn << 5) | (uint32_t) rt;
}

uint32_t a64_strb(A64Reg rt, A64Reg rn, int32_t offset) {
    // STRB Wt, [Xn, #uimm12] (8-bit byte store, offset NOT scaled)
    // 0011 1001 00 imm12 Rn:5 Rt:5 = base 0x39000000
    XR_DCHECK(offset >= 0 && offset < 4096, "assertion failed");
    uint32_t imm12 = (uint32_t) offset;
    return 0x39000000u | (imm12 << 10) | ((uint32_t) rn << 5) | (uint32_t) rt;
}

/* ========== 16-bit Memory ========== */

uint32_t a64_ldrh(A64Reg rt, A64Reg rn, int32_t offset) {
    // LDRH Wt, [Xn, #offset]  (16-bit unsigned load, offset scaled by 2)
    // 0111 1001 01 imm12 Rn:5 Rt:5 = base 0x79400000
    XR_DCHECK(offset >= 0 && (offset % 2) == 0, "assertion failed");
    uint32_t imm12 = (uint32_t) (offset / 2);
    XR_DCHECK(imm12 < 4096, "assertion failed");
    return 0x79400000u | (imm12 << 10) | ((uint32_t) rn << 5) | (uint32_t) rt;
}

uint32_t a64_strh(A64Reg rt, A64Reg rn, int32_t offset) {
    // STRH Wt, [Xn, #offset] (16-bit store, offset scaled by 2)
    // 0111 1001 00 imm12 Rn:5 Rt:5 = base 0x79000000
    XR_DCHECK(offset >= 0 && (offset % 2) == 0, "assertion failed");
    uint32_t imm12 = (uint32_t) (offset / 2);
    XR_DCHECK(imm12 < 4096, "assertion failed");
    return 0x79000000u | (imm12 << 10) | ((uint32_t) rn << 5) | (uint32_t) rt;
}

/* ========== 32-bit Memory ========== */

uint32_t a64_ldr_w(A64Reg rt, A64Reg rn, int32_t offset) {
    // LDR Wt, [Xn, #offset]  (32-bit, unsigned offset scaled by 4)
    // sf=0 | 11_1_00_10_1 | imm12 | Rn | Rt = base 0xB9400000
    XR_DCHECK(offset >= 0 && (offset % 4) == 0, "assertion failed");
    uint32_t imm12 = (uint32_t) (offset / 4);
    XR_DCHECK(imm12 < 4096, "assertion failed");
    return 0xB9400000u | (imm12 << 10) | ((uint32_t) rn << 5) | (uint32_t) rt;
}

uint32_t a64_str_w(A64Reg rt, A64Reg rn, int32_t offset) {
    // STR Wt, [Xn, #offset]  (32-bit, unsigned offset scaled by 4)
    // sf=0 | 11_1_00_10_0 | imm12 | Rn | Rt = base 0xB9000000
    XR_DCHECK(offset >= 0 && (offset % 4) == 0, "assertion failed");
    uint32_t imm12 = (uint32_t) (offset / 4);
    XR_DCHECK(imm12 < 4096, "assertion failed");
    return 0xB9000000u | (imm12 << 10) | ((uint32_t) rn << 5) | (uint32_t) rt;
}

uint32_t a64_subs_imm_w(A64Reg rd, A64Reg rn, uint32_t imm12) {
    // SUBS Wd, Wn, #imm12  (32-bit subtract setting flags)
    // sf=0 | 11 | 10001 | shift=0 | imm12 | Rn | Rd = base 0x71000000
    XR_DCHECK(imm12 < 4096, "assertion failed");
    return 0x71000000u | (imm12 << 10) | ((uint32_t) rn << 5) | (uint32_t) rd;
}

/* ========== Conditional Branch ========== */

uint32_t a64_b_cond(A64Cond cond, int32_t offset_insts) {
    // B.cond offset  (PC-relative conditional branch)
    // 0101010_0 | imm19 | 0 | cond = base 0x54000000
    uint32_t imm19 = (uint32_t) offset_insts & 0x7FFFF;
    return 0x54000000u | (imm19 << 5) | (uint32_t) cond;
}

/* ========== Helper: Load 64-bit Immediate ========== */

int a64_load_imm64(A64Buf *buf, A64Reg rd, uint64_t imm) {
    // Special cases
    if (imm == 0) {
        a64_buf_emit(buf, a64_movz(rd, 0, 0));
        return 1;
    }

    // Count non-zero 16-bit chunks
    uint16_t chunks[4] = {
        (uint16_t) (imm),
        (uint16_t) (imm >> 16),
        (uint16_t) (imm >> 32),
        (uint16_t) (imm >> 48),
    };

    // Find first non-zero chunk for MOVZ
    int first = -1;
    for (int i = 0; i < 4; i++) {
        if (chunks[i] != 0) {
            first = i;
            break;
        }
    }

    a64_buf_emit(buf, a64_movz(rd, chunks[first], (uint8_t) (first * 16)));
    int count = 1;

    // MOVK for remaining non-zero chunks
    for (int i = first + 1; i < 4; i++) {
        if (chunks[i] != 0) {
            a64_buf_emit(buf, a64_movk(rd, chunks[i], (uint8_t) (i * 16)));
            count++;
        }
    }

    return count;
}

/* ========== Floating-Point Arithmetic (double, 64-bit) ========== */

// Double-precision data processing: 0x1E6x_xxxx
// type=01 (double), bits [23:22] = 01

uint32_t a64_fadd(A64Reg rd, A64Reg rn, A64Reg rm) {
    // FADD Dd, Dn, Dm: 0001_1110_011 Rm 0010_10 Rn Rd
    return 0x1E602800 | ((rm & 0x1F) << 16) | ((rn & 0x1F) << 5) | (rd & 0x1F);
}

uint32_t a64_fsub(A64Reg rd, A64Reg rn, A64Reg rm) {
    // FSUB Dd, Dn, Dm: 0001_1110_011 Rm 0011_10 Rn Rd
    return 0x1E603800 | ((rm & 0x1F) << 16) | ((rn & 0x1F) << 5) | (rd & 0x1F);
}

uint32_t a64_fmul(A64Reg rd, A64Reg rn, A64Reg rm) {
    // FMUL Dd, Dn, Dm: 0001_1110_011 Rm 0000_10 Rn Rd
    return 0x1E600800 | ((rm & 0x1F) << 16) | ((rn & 0x1F) << 5) | (rd & 0x1F);
}

uint32_t a64_fdiv(A64Reg rd, A64Reg rn, A64Reg rm) {
    // FDIV Dd, Dn, Dm: 0001_1110_011 Rm 0001_10 Rn Rd
    return 0x1E601800 | ((rm & 0x1F) << 16) | ((rn & 0x1F) << 5) | (rd & 0x1F);
}

uint32_t a64_fneg(A64Reg rd, A64Reg rn) {
    // FNEG Dd, Dn: 0001_1110_0110_0001_0100_00 Rn Rd
    return 0x1E614000 | ((rn & 0x1F) << 5) | (rd & 0x1F);
}

uint32_t a64_fmov(A64Reg rd, A64Reg rn) {
    // FMOV Dd, Dn: 0001_1110_0110_0000_0100_00 Rn Rd
    return 0x1E604000 | ((rn & 0x1F) << 5) | (rd & 0x1F);
}

uint32_t a64_fmov_gp_to_fp(A64Reg dd, A64Reg xn) {
    // FMOV Dd, Xn (64-bit GP→FP): 1001_1110_0110_0111_0000_00 Xn Dd
    return 0x9E670000 | ((xn & 0x1F) << 5) | (dd & 0x1F);
}

uint32_t a64_fcmp(A64Reg rn, A64Reg rm) {
    // FCMP Dn, Dm: 0001_1110_011 Rm 0010_00 Rn 00000
    return 0x1E602000 | ((rm & 0x1F) << 16) | ((rn & 0x1F) << 5);
}

/* ========== Floating-Point Conversion ========== */

uint32_t a64_scvtf(A64Reg rd, A64Reg rn) {
    // SCVTF Dd, Xn: 1001_1110_0110_0010_0000_00 Rn Rd
    return 0x9E620000 | ((rn & 0x1F) << 5) | (rd & 0x1F);
}

uint32_t a64_fcvtzs(A64Reg rd, A64Reg rn) {
    // FCVTZS Xd, Dn: 1001_1110_0111_1000_0000_00 Rn Rd
    return 0x9E780000 | ((rn & 0x1F) << 5) | (rd & 0x1F);
}

/* ========== Floating-Point Move (GPR ↔ FPR) ========== */

uint32_t a64_fmov_from_gpr(A64Reg dd, A64Reg xn) {
    // FMOV Dd, Xn: 1001_1110_0110_0111_0000_00 Xn Dd
    return 0x9E670000 | ((xn & 0x1F) << 5) | (dd & 0x1F);
}

uint32_t a64_fmov_to_gpr(A64Reg xd, A64Reg dn) {
    // FMOV Xd, Dn: 1001_1110_0110_0110_0000_00 Dn Xd
    return 0x9E660000 | ((dn & 0x1F) << 5) | (xd & 0x1F);
}

/* ========== Floating-Point Memory ========== */

uint32_t a64_ldr_fp(A64Reg rt, A64Reg rn, int32_t offset) {
    // LDR Dt, [Xn, #offset]: 1111_1101_01 imm12 Rn Rt
    // offset must be 8-byte aligned, encoded as offset/8
    XR_DCHECK((offset & 7) == 0 && offset >= 0, "assertion failed");
    uint32_t imm12 = (uint32_t) (offset / 8) & 0xFFF;
    return 0xFD400000 | (imm12 << 10) | ((rn & 0x1F) << 5) | (rt & 0x1F);
}

uint32_t a64_str_fp(A64Reg rt, A64Reg rn, int32_t offset) {
    // STR Dt, [Xn, #offset]: 1111_1101_00 imm12 Rn Rt
    XR_DCHECK((offset & 7) == 0 && offset >= 0, "assertion failed");
    uint32_t imm12 = (uint32_t) (offset / 8) & 0xFFF;
    return 0xFD000000 | (imm12 << 10) | ((rn & 0x1F) << 5) | (rt & 0x1F);
}

uint32_t a64_stp_fp(A64Reg rt1, A64Reg rt2, A64Reg rn, int32_t offset) {
    // STP Dt1, Dt2, [Xn, #offset] (64-bit FP pair store)
    // opc=01 V=1 type=010 L=0: base 0x6D000000
    int32_t imm7 = offset / 8;
    XR_DCHECK(imm7 >= -64 && imm7 < 64, "assertion failed");
    return 0x6D000000u | (((uint32_t) imm7 & 0x7F) << 15) | ((rt2 & 0x1F) << 10) |
           ((rn & 0x1F) << 5) | (rt1 & 0x1F);
}

uint32_t a64_ldp_fp(A64Reg rt1, A64Reg rt2, A64Reg rn, int32_t offset) {
    // LDP Dt1, Dt2, [Xn, #offset] (64-bit FP pair load)
    // opc=01 V=1 type=010 L=1: base 0x6D400000
    int32_t imm7 = offset / 8;
    XR_DCHECK(imm7 >= -64 && imm7 < 64, "assertion failed");
    return 0x6D400000u | (((uint32_t) imm7 & 0x7F) << 15) | ((rt2 & 0x1F) << 10) |
           ((rn & 0x1F) << 5) | (rt1 & 0x1F);
}

/* ========== Helper: Load f64 Immediate ========== */

int a64_load_f64(A64Buf *buf, A64Reg dd, A64Reg scratch_gpr, double val) {
    uint64_t bits;
    memcpy(&bits, &val, 8);

    // Special case: positive zero
    if (bits == 0) {
        // FMOV Dd, XZR (move zero from GPR)
        a64_buf_emit(buf, a64_fmov_from_gpr(dd, A64_XZR));
        return 1;
    }

    // General case: load bits into scratch GPR, then FMOV to FPR
    int n = a64_load_imm64(buf, scratch_gpr, bits);
    a64_buf_emit(buf, a64_fmov_from_gpr(dd, scratch_gpr));
    return n + 1;
}

/* ========== NOP ========== */

uint32_t a64_nop(void) {
    return 0xD503201F;
}

#endif  // __aarch64__
