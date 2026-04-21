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
 * PATTERNS (Phase 5.3):
 *   1. STR Xt,[SP,#N] ; LDR Xt,[SP,#N]  → NOP the LDR
 *   2. MOV Xd, Xd  (ORR Xd, XZR, Xd)   → NOP
 *   3. CMP Xn, #0 ; B.EQ/B.NE           → CBZ/CBNZ Xn (fuse)
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

#endif // XIR_PEEPHOLE_H
