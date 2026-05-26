/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xm_arm64.c - ARM64 hand-written residual functions
 *
 * Most encoding functions are auto-generated in xm_arm64_gen.c.
 * This file contains only:
 *   - Code buffer management (buf_init / buf_emit / buf_offset)
 *   - Functions with complex encoding logic (tst_imm, ubfx64)
 *   - Aliases that duplicate a generated function (b_cond, fmov_from_gpr)
 *   - Multi-instruction helpers (load_imm64, load_f64)
 */

#ifdef __aarch64__

#include "xm_arm64.h"
#include "../base/xchecks.h"
#include <string.h>

/* ========== Code Buffer ========== */

void a64_buf_init(A64Buf *buf, uint32_t *mem, uint32_t cap_instructions) {
    buf->code = mem;
    buf->count = 0;
    buf->capacity = cap_instructions;
}

void a64_buf_emit(A64Buf *buf, uint32_t inst) {
    XR_DCHECK(buf->count < buf->capacity, "A64Buf overflow");
    buf->code[buf->count++] = inst;
}

uint32_t a64_buf_offset(A64Buf *buf) {
    return buf->count * 4;
}

/* ========== Complex Encoding ========== */

uint32_t a64_tst_imm(A64Reg rn, uint64_t bitmask_imm) {
    /* TST Xn, #imm = ANDS XZR, Xn, #imm
     * Supports (2^n - 1) masks: 0x1, 0x3, 0x7, 0xF, 0x1F, 0x3F
     * For 64-bit element (N=1, immr=0), imms = number_of_ones - 1 */
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

uint32_t a64_ubfx64(A64Reg rd, A64Reg rn, uint32_t lsb, uint32_t width) {
    /* UBFX Xd, Xn, #lsb, #width  →  UBFM Xd, Xn, #lsb, #(lsb+width-1) */
    XR_DCHECK(lsb + width <= 64 && width > 0, "UBFX range overflow");
    uint32_t imms = lsb + width - 1;
    return 0xD3400000u | (lsb << 16) | (imms << 10) | ((uint32_t) rn << 5) | (uint32_t) rd;
}

/* ========== Aliases ========== */

uint32_t a64_b_cond(A64Cond cond, int32_t offset_insts) {
    return a64_bcond(cond, offset_insts);
}

uint32_t a64_fmov_from_gpr(A64Reg dd, A64Reg xn) {
    return a64_fmov_gp_to_fp(dd, xn);
}

/* ========== Multi-Instruction Helpers ========== */

int a64_load_imm64(A64Buf *buf, A64Reg rd, uint64_t imm) {
    if (imm == 0) {
        a64_buf_emit(buf, a64_movz(rd, 0, 0));
        return 1;
    }

    uint16_t chunks[4] = {
        (uint16_t) (imm),
        (uint16_t) (imm >> 16),
        (uint16_t) (imm >> 32),
        (uint16_t) (imm >> 48),
    };

    int first = -1;
    for (int i = 0; i < 4; i++) {
        if (chunks[i] != 0) {
            first = i;
            break;
        }
    }
    XR_DCHECK(first >= 0, "unreachable: imm != 0 but no chunk set");

    a64_buf_emit(buf, a64_movz(rd, chunks[first], (uint8_t) (first * 16)));
    int count = 1;

    for (int i = first + 1; i < 4; i++) {
        if (chunks[i] != 0) {
            a64_buf_emit(buf, a64_movk(rd, chunks[i], (uint8_t) (i * 16)));
            count++;
        }
    }

    return count;
}

int a64_load_f64(A64Buf *buf, A64Reg dd, A64Reg scratch_gpr, double val) {
    uint64_t bits;
    memcpy(&bits, &val, 8);

    if (bits == 0) {
        a64_buf_emit(buf, a64_fmov_from_gpr(dd, A64_XZR));
        return 1;
    }

    int n = a64_load_imm64(buf, scratch_gpr, bits);
    a64_buf_emit(buf, a64_fmov_from_gpr(dd, scratch_gpr));
    return n + 1;
}

#endif  // __aarch64__
