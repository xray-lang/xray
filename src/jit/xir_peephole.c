/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_peephole.c - Post-codegen ARM64 peephole optimiser.
 *
 * DESIGN:
 *   A single forward scan replaces matched patterns with NOPs in-place.
 *   Because instruction count and addresses never change, all branch
 *   offsets, block-start tables, and deopt metadata stay valid.
 *
 *   Each pattern matcher is a small static function returning the number
 *   of instructions consumed (0 = no match).  The driver calls them in
 *   priority order at each position.
 */

#include "xir_peephole.h"
#include "../base/xchecks.h"

/* ========== ARM64 encoding helpers ========== */

#define A64_NOP 0xD503201Fu

/* STR Xt, [Xn, #off] unsigned-offset 64-bit:
 *   11_111_0_01_00_imm12_Rn_Rt  →  mask top 10 bits = 0xFFC00000
 *   base = 0xF9000000 */
static inline bool is_str_x(uint32_t w) {
    return (w & 0xFFC00000u) == 0xF9000000u;
}

/* LDR Xt, [Xn, #off] unsigned-offset 64-bit:
 *   base = 0xF9400000 */
static inline bool is_ldr_x(uint32_t w) {
    return (w & 0xFFC00000u) == 0xF9400000u;
}

/* Extract Rt (bits 4:0), Rn (bits 9:5), imm12 (bits 21:10). */
static inline uint32_t enc_rt(uint32_t w)    { return w & 0x1Fu; }
static inline uint32_t enc_rn(uint32_t w)    { return (w >> 5) & 0x1Fu; }
static inline uint32_t enc_imm12(uint32_t w) { return (w >> 10) & 0xFFFu; }

/* MOV Xd, Xn = ORR Xd, XZR, Xn (no shift):
 *   1_01_01010_00_0_Rm_000000_11111_Rd
 *   Fixed bits mask: 0xFFE0FFE0, base: 0xAA0003E0 */
static inline bool is_mov_x(uint32_t w) {
    return (w & 0xFFE0FFE0u) == 0xAA0003E0u;
}
static inline uint32_t mov_rd(uint32_t w)  { return w & 0x1Fu; }
static inline uint32_t mov_rm(uint32_t w)  { return (w >> 16) & 0x1Fu; }

/* SUBS XZR, Xn, #imm12  (CMP Xn, #imm12):
 *   1_1_1_10001_0_0_imm12_Rn_11111
 *   Top 10 bits (sf|op|S|10001|sh|0): 0xF100001F with Rd=XZR(31)
 *   mask=0xFF00001F, base=0xF100001F */
static inline bool is_cmp_imm(uint32_t w) {
    return (w & 0xFF00001Fu) == 0xF100001Fu;
}
static inline uint32_t cmp_rn(uint32_t w)    { return (w >> 5) & 0x1Fu; }
static inline uint32_t cmp_imm(uint32_t w)   { return (w >> 10) & 0xFFFu; }

/* B.cond: 0101010_0_imm19_0_cond
 *   mask top 8 bits: 0xFF000010, base: 0x54000000 */
static inline bool is_bcond(uint32_t w) {
    return (w & 0xFF000010u) == 0x54000000u;
}
static inline uint32_t bcond_cond(uint32_t w) { return w & 0xFu; }
/* Extract signed imm19 (bits 23:5) */
static inline int32_t bcond_offset(uint32_t w) {
    int32_t raw = (int32_t)((w >> 5) & 0x7FFFFu);
    if (raw & 0x40000) raw |= (int32_t)0xFFF80000; /* sign-extend */
    return raw;
}

/* A64_CC_EQ = 0, A64_CC_NE = 1 */
#define COND_EQ 0u
#define COND_NE 1u

/* CBZ  Xt, offset: 1_011010_0_imm19_Rt  →  base 0xB4000000 */
static inline uint32_t make_cbz(uint32_t rt, int32_t off19) {
    return 0xB4000000u | (((uint32_t)off19 & 0x7FFFFu) << 5) | rt;
}

/* CBNZ Xt, offset: 1_011010_1_imm19_Rt  →  base 0xB5000000 */
static inline uint32_t make_cbnz(uint32_t rt, int32_t off19) {
    return 0xB5000000u | (((uint32_t)off19 & 0x7FFFFu) << 5) | rt;
}

/* STR Wt, [Xn, #off] unsigned-offset 32-bit:
 *   10_111_0_01_00_imm12_Rn_Rt  →  mask top 10 bits = 0xFFC00000
 *   base = 0xB9000000 */
static inline bool is_str_w(uint32_t w) {
    return (w & 0xFFC00000u) == 0xB9000000u;
}

/* LDR Wt, [Xn, #off] unsigned-offset 32-bit:
 *   base = 0xB9400000 */
static inline bool is_ldr_w(uint32_t w) {
    return (w & 0xFFC00000u) == 0xB9400000u;
}

/* STP Xt1, Xt2, [Xn, #imm7] pre/post/offset 64-bit:
 *   x_010_1001_0_0_imm7_Rt2_Rn_Rt1 (offset form, opc=10)
 *   base = 0xA9000000 */
static inline uint32_t make_stp_x(uint32_t rt1, uint32_t rt2,
                                   uint32_t rn, int32_t imm7) {
    return 0xA9000000u | (((uint32_t)imm7 & 0x7Fu) << 15) |
           (rt2 << 10) | (rn << 5) | rt1;
}

/* LDP Xt1, Xt2, [Xn, #imm7] offset 64-bit:
 *   base = 0xA9400000 */
static inline uint32_t make_ldp_x(uint32_t rt1, uint32_t rt2,
                                   uint32_t rn, int32_t imm7) {
    return 0xA9400000u | (((uint32_t)imm7 & 0x7Fu) << 15) |
           (rt2 << 10) | (rn << 5) | rt1;
}

/* SUBS Xd, Xn, Xm (sets flags): 1_1_1_01011_00_0_Rm_000000_Rn_Rd
 * mask = 0xFFE0FC00, base = 0xEB000000 */
static inline bool is_subs_reg(uint32_t w) {
    return (w & 0xFFE0FC00u) == 0xEB000000u;
}
static inline uint32_t subs_rd(uint32_t w)  { return w & 0x1Fu; }
static inline uint32_t subs_rn(uint32_t w)  { return (w >> 5) & 0x1Fu; }
static inline uint32_t subs_rm(uint32_t w)  { return (w >> 16) & 0x1Fu; }

/* CMP Xn, Xm (SUBS XZR, Xn, Xm): same encoding but Rd=31 */
static inline bool is_cmp_reg(uint32_t w) {
    return is_subs_reg(w) && subs_rd(w) == 31;
}

/* B (unconditional): 000101_imm26, base = 0x14000000 */
static inline bool is_b(uint32_t w) {
    return (w & 0xFC000000u) == 0x14000000u;
}
static inline int32_t b_offset(uint32_t w) {
    int32_t raw = (int32_t)(w & 0x3FFFFFFu);
    if (raw & 0x2000000) raw |= (int32_t)0xFC000000; /* sign-extend */
    return raw;
}

/* Invert B.cond: flip bit 0 (EQ↔NE, LT↔GE, etc.) */
static inline uint32_t invert_cond(uint32_t cond) { return cond ^ 1u; }

/* Build B.cond instruction */
static inline uint32_t make_bcond(uint32_t cond, int32_t off19) {
    return 0x54000000u | (((uint32_t)off19 & 0x7FFFFu) << 5) | cond;
}

/* MOVZ Xd, #imm16, LSL #shift:
 * 1_10_100101_hw_imm16_Rd  →  mask=0xFF800000, base varies by hw
 * hw=0: 0xD2800000, hw=1: 0xD2A00000, hw=2: 0xD2C00000, hw=3: 0xD2E00000 */
static inline bool is_movz_x(uint32_t w) {
    return (w & 0x7F800000u) == 0x52800000u && (w & 0x80000000u);
}
static inline uint32_t movz_rd(uint32_t w) { return w & 0x1Fu; }
static inline uint32_t movz_hw(uint32_t w) { return (w >> 21) & 0x3u; }
static inline uint32_t movz_imm16(uint32_t w) { return (w >> 5) & 0xFFFFu; }

/* MOVK Xd, #imm16, LSL #shift:
 * 1_11_100101_hw_imm16_Rd  →  top bit set + opc=11 */
static inline bool is_movk_x(uint32_t w) {
    return (w & 0x7F800000u) == 0x72800000u && (w & 0x80000000u);
}
static inline uint32_t movk_rd(uint32_t w) { return w & 0x1Fu; }
static inline uint32_t movk_hw(uint32_t w) { return (w >> 21) & 0x3u; }
static inline uint32_t movk_imm16(uint32_t w) { return (w >> 5) & 0xFFFFu; }

/* ========== Pattern 1: STR+LDR → NOP the LDR ========== */

static uint32_t try_str_ldr(uint32_t *code, uint32_t i, uint32_t n) {
    if (i + 1 >= n) return 0;
    uint32_t a = code[i], b = code[i + 1];
    /* 64-bit STR+LDR */
    if (is_str_x(a) && is_ldr_x(b) &&
        enc_rt(a) == enc_rt(b) &&
        enc_rn(a) == enc_rn(b) &&
        enc_imm12(a) == enc_imm12(b)) {
        code[i + 1] = A64_NOP;
        return 1;
    }
    /* 32-bit STR+LDR */
    if (is_str_w(a) && is_ldr_w(b) &&
        enc_rt(a) == enc_rt(b) &&
        enc_rn(a) == enc_rn(b) &&
        enc_imm12(a) == enc_imm12(b)) {
        code[i + 1] = A64_NOP;
        return 1;
    }
    return 0;
}

/* ========== Pattern 2: MOV Xd, Xd → NOP ========== */

static uint32_t try_mov_self(uint32_t *code, uint32_t i, uint32_t n) {
    (void)n;
    uint32_t w = code[i];
    if (is_mov_x(w) && mov_rd(w) == mov_rm(w)) {
        code[i] = A64_NOP;
        return 1;
    }
    return 0;
}

/* ========== Pattern 3: CMP Xn,#0 + B.EQ/B.NE → CBZ/CBNZ ========== */

static uint32_t try_cmp_cbz(uint32_t *code, uint32_t i, uint32_t n) {
    if (i + 1 >= n) return 0;
    uint32_t a = code[i], b = code[i + 1];
    if (!is_cmp_imm(a) || cmp_imm(a) != 0) return 0;
    if (!is_bcond(b)) return 0;

    uint32_t cond = bcond_cond(b);
    if (cond != COND_EQ && cond != COND_NE) return 0;

    uint32_t rn = cmp_rn(a);
    int32_t  off = bcond_offset(b);

    /* The branch offset is relative to the B.cond instruction (i+1).
     * CBZ/CBNZ will occupy slot i, so its offset must be adjusted by +1
     * (because it sits one instruction earlier than the original B.cond). */
    int32_t new_off = off + 1;
    /* imm19 range: ±256 KiB (±2^18 instructions) */
    if (new_off < -(1 << 18) || new_off >= (1 << 18)) return 0;

    code[i] = (cond == COND_EQ) ? make_cbz(rn, new_off)
                                 : make_cbnz(rn, new_off);
    code[i + 1] = A64_NOP;
    return 1;
}

/* ========== Pattern 4: LDP/STP fusion ========== */
/* Adjacent STR Xt1,[Xn,#off] ; STR Xt2,[Xn,#off+8] → STP Xt1,Xt2,[Xn,#off]
 * Adjacent LDR Xt1,[Xn,#off] ; LDR Xt2,[Xn,#off+8] → LDP Xt1,Xt2,[Xn,#off]
 * imm12 for STR/LDR is in units of 8 bytes (64-bit); STP/LDP imm7 is in
 * units of 8 bytes too but signed 7-bit. */

static uint32_t try_ldp_stp(uint32_t *code, uint32_t i, uint32_t n) {
    if (i + 1 >= n) return 0;
    uint32_t a = code[i], b = code[i + 1];

    /* STR+STR fusion → STP */
    if (is_str_x(a) && is_str_x(b) &&
        enc_rn(a) == enc_rn(b) &&
        enc_rt(a) != enc_rt(b) &&  /* different registers */
        enc_imm12(b) == enc_imm12(a) + 1) {  /* consecutive 8-byte slots */
        uint32_t rn = enc_rn(a);
        uint32_t off = enc_imm12(a);  /* in 8-byte units */
        /* STP imm7 range: [-64, 63] in 8-byte units */
        if (off <= 63) {
            code[i] = make_stp_x(enc_rt(a), enc_rt(b), rn, (int32_t)off);
            code[i + 1] = A64_NOP;
            return 1;
        }
    }

    /* LDR+LDR fusion → LDP */
    if (is_ldr_x(a) && is_ldr_x(b) &&
        enc_rn(a) == enc_rn(b) &&
        enc_rt(a) != enc_rt(b) &&
        enc_imm12(b) == enc_imm12(a) + 1) {
        uint32_t rn = enc_rn(a);
        uint32_t off = enc_imm12(a);
        if (off <= 63) {
            code[i] = make_ldp_x(enc_rt(a), enc_rt(b), rn, (int32_t)off);
            code[i + 1] = A64_NOP;
            return 1;
        }
    }
    return 0;
}

/* ========== Pattern 5: Redundant flag / CMP elimination ========== */
/* SUBS Xd, Xn, Xm ; CMP Xn, Xm (SUBS XZR, Xn, Xm)
 * If SUBS already set flags from the same operands, the CMP is redundant. */

static uint32_t try_redundant_cmp(uint32_t *code, uint32_t i, uint32_t n) {
    if (i + 1 >= n) return 0;
    uint32_t a = code[i], b = code[i + 1];
    if (!is_subs_reg(a) || !is_cmp_reg(b)) return 0;
    /* Same source operands: SUBS Xd,Xn,Xm followed by CMP Xn,Xm */
    if (subs_rn(a) == subs_rn(b) && subs_rm(a) == subs_rm(b)) {
        code[i + 1] = A64_NOP;
        return 1;
    }
    return 0;
}

/* ========== Pattern 6: MOVZ+MOVK simplification ========== */
/* MOVZ Xd, #0 ; MOVK Xd, #imm, LSL #0  → MOVZ Xd, #imm
 * (common for small constants built in two steps) */

static uint32_t try_movz_movk(uint32_t *code, uint32_t i, uint32_t n) {
    if (i + 1 >= n) return 0;
    uint32_t a = code[i], b = code[i + 1];
    if (!is_movz_x(a) || !is_movk_x(b)) return 0;
    if (movz_rd(a) != movk_rd(b)) return 0;
    /* MOVZ Xd,#0,LSL#0 + MOVK Xd,#imm16,LSL#0 → MOVZ Xd,#imm16 */
    if (movz_hw(a) == 0 && movz_imm16(a) == 0 && movk_hw(b) == 0) {
        /* Replace first with MOVZ Xd, #imm16 */
        uint32_t rd = movz_rd(a);
        uint32_t imm16 = movk_imm16(b);
        code[i] = 0xD2800000u | (imm16 << 5) | rd;
        code[i + 1] = A64_NOP;
        return 1;
    }
    return 0;
}

/* ========== Pattern 7: B.cond over B → inverted B.cond ========== */
/* B.cond +2 ; B target → B.!cond target
 * (B.cond skips one instruction which is the unconditional branch) */

static uint32_t try_bcond_over_b(uint32_t *code, uint32_t i, uint32_t n) {
    if (i + 2 >= n) return 0;
    uint32_t a = code[i], b = code[i + 1];
    if (!is_bcond(a) || !is_b(b)) return 0;
    /* B.cond must jump exactly +2 instructions (skip the B) */
    if (bcond_offset(a) != 2) return 0;
    /* Build inverted B.cond to the B target.
     * B target offset is relative to i+1; new B.cond is at i,
     * so adjust offset by +1. */
    int32_t b_off = b_offset(b);
    int32_t new_off = b_off + 1;
    /* Check imm19 range */
    if (new_off < -(1 << 18) || new_off >= (1 << 18)) return 0;
    code[i] = make_bcond(invert_cond(bcond_cond(a)), new_off);
    code[i + 1] = A64_NOP;
    return 1;
}

/* ========== Pattern 8: STR immediately followed by same STR ========== */
/* STR Xt,[Xn,#off] ; STR Xt,[Xn,#off] → NOP the first STR
 * (codegen can emit duplicate stores from SSA phi resolution) */

static uint32_t try_dup_str(uint32_t *code, uint32_t i, uint32_t n) {
    if (i + 1 >= n) return 0;
    uint32_t a = code[i], b = code[i + 1];
    if (a == b && (is_str_x(a) || is_str_w(a))) {
        code[i] = A64_NOP;
        return 1;
    }
    return 0;
}

/* ========== Driver ========== */

uint32_t xir_peephole(A64Buf *buf) {
    XR_DCHECK(buf != NULL, "xir_peephole: NULL buf");
    if (!buf->code || buf->count < 2) return 0;

    uint32_t *code = buf->code;
    uint32_t  n    = buf->count;
    uint32_t  nops = 0;

    for (uint32_t i = 0; i < n; i++) {
        uint32_t r;
        /* Try patterns in priority order (most impactful first). */
        r = try_ldp_stp(code, i, n);       if (r) { nops += r; i++; continue; }
        r = try_str_ldr(code, i, n);       if (r) { nops += r; continue; }
        r = try_dup_str(code, i, n);       if (r) { nops += r; continue; }
        r = try_redundant_cmp(code, i, n); if (r) { nops += r; i++; continue; }
        r = try_cmp_cbz(code, i, n);       if (r) { nops += r; i++; continue; }
        r = try_bcond_over_b(code, i, n);  if (r) { nops += r; i++; continue; }
        r = try_movz_movk(code, i, n);     if (r) { nops += r; i++; continue; }
        r = try_mov_self(code, i, n);      if (r) { nops += r; continue; }
    }

    return nops;
}
