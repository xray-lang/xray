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

/* ========== Pattern 1: STR+LDR → NOP the LDR ========== */

static uint32_t try_str_ldr(uint32_t *code, uint32_t i, uint32_t n) {
    if (i + 1 >= n) return 0;
    uint32_t a = code[i], b = code[i + 1];
    if (!is_str_x(a) || !is_ldr_x(b)) return 0;
    /* Same Rt, Rn, imm12 → redundant reload. */
    if (enc_rt(a) == enc_rt(b) &&
        enc_rn(a) == enc_rn(b) &&
        enc_imm12(a) == enc_imm12(b)) {
        code[i + 1] = A64_NOP;
        return 1;  /* consumed 1 NOP */
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
        r = try_str_ldr(code, i, n);   if (r) { nops += r; continue; }
        r = try_cmp_cbz(code, i, n);   if (r) { nops += r; i++; continue; }
        r = try_mov_self(code, i, n);   if (r) { nops += r; continue; }
    }

    return nops;
}
