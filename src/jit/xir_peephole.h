/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_peephole.h - Post-codegen ARM64 peephole optimiser.
 *
 * KEY CONCEPT:
 *   After all instructions have been emitted and branch offsets patched,
 *   a single pass over ctx->buf.code[] replaces redundant instruction
 *   patterns with NOPs.  Because NOPs preserve instruction count and
 *   alignment, every branch offset and block-start table remains valid.
 *
 * PATTERNS:
 *   1. STR Xt,[Xn,#N] ; LDR Xt,[Xn,#N]  → NOP the LDR  (32/64-bit)
 *   2. MOV Xd, Xd  (ORR Xd, XZR, Xd)   → NOP
 *   3. CMP Xn, #0 ; B.EQ/B.NE           → CBZ/CBNZ Xn (fuse)
 *   4. STR+STR / LDR+LDR adjacent        → STP/LDP (pair fusion)
 *   5. SUBS Xd,Xn,Xm ; CMP Xn,Xm       → NOP the CMP (redundant flags)
 *   6. MOVZ Xd,#0 ; MOVK Xd,#imm        → MOVZ Xd,#imm
 *   7. B.cond +2 ; B target              → B.!cond target
 *   8. STR Xt,[Xn,#N] ; STR Xt,[Xn,#N]  → NOP duplicate store
 */

#ifndef XIR_PEEPHOLE_H
#define XIR_PEEPHOLE_H

#include "xir_arm64.h"
#include "../base/xdefs.h"

/*
 * Run the peephole pass over an already-patched instruction buffer.
 * Returns the number of instructions NOP-ed out (for diagnostics).
 */
XR_FUNC uint32_t xir_peephole(A64Buf *buf);

#endif  // XIR_PEEPHOLE_H
