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
 *
 * Per-opcode metadata now carries a field_kind triple sourced from
 * the KOP_* shorthand in xopcode_def.h. The triple is the runtime
 * contract every opcode publishes (which slot is a register / a K
 * index / a literal / unused), letting the emitter validate operands
 * before they are encoded into an instruction word.
 */

#include "xopcode_info.h"
#include "xopcode_def.h"
#include "../../base/xchecks.h"

static const XrOpCodeInfo opcode_table[NUM_OPCODES] = {
#define _XR_OPCODE_INFO(name, fmt, kop, desc) [OP_##name] = {#name, fmt, {kop}, desc},
    XR_OPCODE_TABLE(_XR_OPCODE_INFO)
#undef _XR_OPCODE_INFO
};

static const XrOpCodeInfo k_unknown_info = {
    "UNKNOWN", FMT_SPECIAL, {XR_OPF_SPECIAL, XR_OPF_SPECIAL, XR_OPF_SPECIAL}, NULL};

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

const char *xr_opcode_field_kind_name(XrOpFieldKind kind) {
    switch (kind) {
        case XR_OPF_NONE:
            return "NONE";
        case XR_OPF_REG_OUT:
            return "REG_OUT";
        case XR_OPF_REG_IN:
            return "REG_IN";
        case XR_OPF_REG_INOUT:
            return "REG_INOUT";
        case XR_OPF_REG_BASE:
            return "REG_BASE";
        case XR_OPF_LIT:
            return "LIT";
        case XR_OPF_LIT_S:
            return "LIT_S";
        case XR_OPF_LIT_FLAG:
            return "LIT_FLAG";
        case XR_OPF_K_IDX:
            return "K_IDX";
        case XR_OPF_SYMBOL_IDX:
            return "SYMBOL_IDX";
        case XR_OPF_PROTO_IDX:
            return "PROTO_IDX";
        case XR_OPF_GLOBAL_IDX:
            return "GLOBAL_IDX";
        case XR_OPF_BUILTIN_IDX:
            return "BUILTIN_IDX";
        case XR_OPF_JUMP:
            return "JUMP";
        case XR_OPF_SUB_OPCODE:
            return "SUB_OPCODE";
        case XR_OPF_SPECIAL:
            return "SPECIAL";
    }
    return "?";
}
