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

// Per-field semantic kind. The OpCode encoding is a tuple of three bytes
// (or two with Bx merging B+C); this enum classifies what each byte
// *means* at runtime so the emitter can validate operands at the
// instruction-construction site instead of waiting for VM dispatch to
// blow up. Together with InstrFormat it forms the contract of an opcode.
typedef enum {
    XR_OPF_NONE = 0,    // unused; must be 0 in this slot
    XR_OPF_REG_OUT,     // destination register: VM writes R[slot]
    XR_OPF_REG_IN,      // source register: VM reads R[slot]
    XR_OPF_REG_INOUT,   // both read and written
    XR_OPF_REG_BASE,    // base register; VM accesses R[slot..slot+N-1]
    XR_OPF_LIT,         // unsigned literal value (0..255 in ABC)
    XR_OPF_LIT_S,       // signed literal (sB / sC / sBx)
    XR_OPF_LIT_FLAG,    // 0 or 1 only
    XR_OPF_K_IDX,       // constant table index (typically Bx)
    XR_OPF_SYMBOL_IDX,  // proto->symbols index (property/method name dispatch)
    XR_OPF_PROTO_IDX,   // sub-proto index (Bx)
    XR_OPF_GLOBAL_IDX,  // shared/global index (Bx)
    XR_OPF_JUMP,        // sJ relative offset
    XR_OPF_SUB_OPCODE,  // composite sub-operation discriminator
    XR_OPF_SPECIAL,     // composite/encoded; skip standard range checks
} XrOpFieldKind;

// Per-opcode metadata.
//
// Field semantics layout: field_kind[0..2] always corresponds to the
// physical A/B/C byte slots in the encoded instruction. For ABx /
// AsBx formats, B+C are merged into a single Bx, and field_kind[1]
// records the Bx semantic while field_kind[2] is NONE. For sJ-only
// formats (JMP) all three slots are NONE; the kind is conveyed by
// the InstrFormat tag.
typedef struct {
    const char *name;
    InstrFormat format;
    XrOpFieldKind field_kind[3];
    const char *desc;
} XrOpCodeInfo;

// Query APIs. Falls back to "UNKNOWN" / FMT_SPECIAL for out-of-range values.
XR_FUNC const XrOpCodeInfo *xr_opcode_info(OpCode op);

// Field-kind helper: human-readable name for diagnostics.
XR_FUNC const char *xr_opcode_field_kind_name(XrOpFieldKind kind);

// Convenience: opcode name only (already declared by xchunk.h, re-exported here).
// The canonical implementation lives in xopcode_info.c.
// Included by xchunk.h via indirect include through shared headers.

#endif  // XOPCODE_INFO_H
