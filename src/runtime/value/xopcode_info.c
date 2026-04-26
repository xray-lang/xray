/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xopcode_info.c - Instruction metadata table.
 *
 * The table is generated from XR_OPCODE_TABLE in xopcode_def.h, the
 * single source of truth shared with the OpCode enum (xchunk.h) and
 * the computed-goto label array (xvm_jumptab.h). Adding a new opcode
 * means editing one X-macro entry there; this file recompiles in
 * lockstep automatically.
 */

#include "xopcode_info.h"
#include "xopcode_def.h"
#include "../../base/xchecks.h"

static const XrOpCodeInfo opcode_table[NUM_OPCODES] = {
#define _XR_OPCODE_INFO(name, fmt, desc) \
    [OP_##name] = { #name, fmt, desc },
    XR_OPCODE_TABLE(_XR_OPCODE_INFO)
#undef _XR_OPCODE_INFO
};

static const XrOpCodeInfo k_unknown_info = {"UNKNOWN", FMT_SPECIAL, NULL};

const XrOpCodeInfo *xr_opcode_info(OpCode op) {
    XR_DCHECK(sizeof(opcode_table) / sizeof(opcode_table[0]) == NUM_OPCODES,
              "opcode_table size mismatch with NUM_OPCODES");
    if (op >= 0 && op < NUM_OPCODES && opcode_table[op].name != NULL) {
        return &opcode_table[op];
    }
    return &k_unknown_info;
}

const char *xr_opcode_name(OpCode op) {
    return xr_opcode_info(op)->name;
}
