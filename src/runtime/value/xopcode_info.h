/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xopcode_info.h - Instruction metadata (name, operand format, description)
 *
 * KEY CONCEPT:
 *   Single source of truth for per-opcode metadata. Located in the value
 *   layer next to OpCode so both runtime and tooling (disassembler, JIT)
 *   consume it without reverse dependencies.
 */

#ifndef XOPCODE_INFO_H
#define XOPCODE_INFO_H

#include "../../base/xdefs.h"
#include "xchunk.h"

// Instruction operand format (how the disassembler prints A/B/C/Bx/sJ).
typedef enum {
    FMT_NONE,
    FMT_A,
    FMT_AB,
    FMT_ABC,
    FMT_ABx,
    FMT_AsBx,
    FMT_sJ,
    FMT_AB_sC,
    FMT_AsB_C,
    FMT_AB_IMM,
    FMT_ABx_INT,  // R[A] <integer Bx> — Bx is a raw integer, not a const index
    FMT_PROTO,
    FMT_GLOBAL,
    FMT_SPECIAL
} InstrFormat;

// Per-opcode metadata.
typedef struct {
    const char *name;
    InstrFormat format;
    const char *desc;
} XrOpCodeInfo;

// Query APIs. Falls back to "UNKNOWN" / FMT_SPECIAL for out-of-range values.
XR_FUNC const XrOpCodeInfo *xr_opcode_info(OpCode op);

// Convenience: opcode name only (already declared by xchunk.h, re-exported here).
// The canonical implementation lives in xopcode_info.c.
// Included by xchunk.h via indirect include through shared headers.

#endif // XOPCODE_INFO_H
