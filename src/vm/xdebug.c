/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xdebug.c - Bytecode debugging tools implementation
 *
 * KEY CONCEPT:
 *   Table-driven bytecode disassembler for debugging and analysis.
 */

#include "xdebug.h"
#include "../base/xchecks.h"
#include "../runtime/object/xstring.h"
#include "../runtime/value/xvalue_print.h"
#include "../runtime/value/xtype.h"
#include "../runtime/value/xslot_type.h"
#include "../runtime/value/xopcode_info.h"
#include <stdio.h>

/* Instruction format enum, OpCodeInfo struct and opcode_table now live in
 * runtime/value/xopcode_info.{h,c}. The disassembler below queries them via
 * xr_opcode_info(). */

/* ========== Formatting Output Functions ========== */

// Format: no operand
static int disasm_none(const char *name, int offset) {
    printf("%-16s\n", name);
    return offset + 1;
}

// Format: R[A]
static int disasm_a(const char *name, XrInstruction inst, int offset) {
    uint8_t a = GETARG_A(inst);
    printf("%-16s R[%d]\n", name, a);
    return offset + 1;
}

// Format: R[A] K[Bx] (constant)
static int disasm_abx(const char *name, XrProto *proto, XrInstruction inst, int offset) {
    uint8_t a = GETARG_A(inst);
    uint16_t bx = GETARG_Bx(inst);

    printf("%-16s R[%d] K[%d] ; ", name, a, bx);

    // Print constant value
    if (bx < PROTO_CONST_COUNT(proto)) {
        xr_value_print(PROTO_CONSTANT(proto, bx));
    } else {
        printf("???");
    }
    printf("\n");

    return offset + 1;
}

// Format: R[A] G[Bx] (global variable)
static int disasm_abx_global(const char *name, XrProto *proto, XrInstruction inst, int offset) {
    (void) proto;
    uint8_t a = GETARG_A(inst);
    uint16_t bx = GETARG_Bx(inst);

    printf("%-16s R[%d] G[%d]\n", name, a, bx);

    return offset + 1;
}

// Format: R[A] XrProto[Bx] (closure)
static int disasm_proto(const char *name, XrProto *proto, XrInstruction inst, int offset) {
    uint8_t a = GETARG_A(inst);
    uint16_t bx = GETARG_Bx(inst);

    printf("%-16s R[%d] XrProto[%d]", name, a, bx);

    // Print function name (if any)
    if (bx < PROTO_PROTO_COUNT(proto)) {
        XrProto *child = PROTO_PROTO(proto, bx);
        if (child != NULL && child->name != NULL) {
            printf(" ; \"%s\"", child->name->data);
        }
    }
    printf("\n");

    return offset + 1;
}

// Format: R[A] R[B] R[C]
static int disasm_abc(const char *name, XrInstruction inst, int offset) {
    uint8_t a = GETARG_A(inst);
    uint8_t b = GETARG_B(inst);
    uint8_t c = GETARG_C(inst);

    printf("%-16s R[%d] R[%d] R[%d]\n", name, a, b, c);
    return offset + 1;
}

// Format: R[A] R[B]
static int disasm_ab(const char *name, XrInstruction inst, int offset) {
    uint8_t a = GETARG_A(inst);
    uint8_t b = GETARG_B(inst);

    printf("%-16s R[%d] R[%d]\n", name, a, b);
    return offset + 1;
}

// Format: R[A] <Bx as raw integer> (Bx is a direct integer, not a const index)
static int disasm_abx_int(const char *name, XrInstruction inst, int offset) {
    uint8_t a = GETARG_A(inst);
    uint16_t bx = GETARG_Bx(inst);
    printf("%-16s R[%d] %d\n", name, a, (int) bx);
    return offset + 1;
}

// Format: R[A] B (B as immediate)
static int disasm_ab_imm(const char *name, XrInstruction inst, int offset) {
    uint8_t a = GETARG_A(inst);
    uint8_t b = GETARG_B(inst);
    uint8_t c = GETARG_C(inst);

    // Check if comparison instruction (need to show k flag)
    OpCode op = GET_OPCODE(inst);
    if (op == OP_EQ || op == OP_EQK || op == OP_LT || op == OP_LE || op == OP_TEST) {
        printf("%-16s R[%d] R[%d] k=%d\n", name, a, b, c);
    } else {
        printf("%-16s R[%d] %d\n", name, a, b);
    }
    return offset + 1;
}

// Format: R[A] sBx (signed immediate)
static int disasm_asbx(const char *name, XrInstruction inst, int offset) {
    uint8_t a = GETARG_A(inst);
    int sbx = GETARG_sBx(inst);

    printf("%-16s R[%d] %d\n", name, a, sbx);
    return offset + 1;
}

// Format: R[A] R[B] sC (B is register, C is signed immediate)
static int disasm_ab_sc(const char *name, XrInstruction inst, int offset) {
    uint8_t a = GETARG_A(inst);
    uint8_t b = GETARG_B(inst);
    int sc = GETARG_sC(inst);

    printf("%-16s R[%d] R[%d] %d\n", name, a, b, sc);
    return offset + 1;
}

// Format: R[A] sB C (B is signed immediate, C is condition flag)
static int disasm_asb_c(const char *name, XrInstruction inst, int offset) {
    uint8_t a = GETARG_A(inst);
    int sb = GETARG_sB(inst);
    uint8_t k = GETARG_C(inst);

    printf("%-16s R[%d] %d k=%d\n", name, a, sb, k);
    return offset + 1;
}

// Format: sJ (jump)
static int disasm_sj(const char *name, XrInstruction inst, int offset) {
    int sj = GETARG_sJ(inst);
    int target = offset + 1 + sj;

    printf("%-16s %d -> %d\n", name, sj, target);
    return offset + 1;
}

// Special format handling (OP_TRY, etc.)
static int disasm_special(const char *name, XrProto *proto, XrInstruction inst, int offset) {
    OpCode op = GET_OPCODE(inst);

    if (op == OP_TRY) {
        // OP_TRY is followed by an instruction containing finally offset
        uint16_t catch_offset = GETARG_Bx(inst);
        printf("%-16s catch=%d ", name, catch_offset);

        // Read next instruction (finally offset)
        if (offset + 1 < PROTO_CODE_COUNT(proto)) {
            XrInstruction next_inst = PROTO_CODE(proto, offset + 1);
            uint16_t finally_offset = GETARG_Bx(next_inst);
            printf("finally=%d\n", finally_offset);
        }
        return offset + 2;
    }

    // NOP with spawn metadata
    if (op == OP_NOP) {
        uint8_t a = GETARG_A(inst);
        uint16_t bx = GETARG_Bx(inst);
        if (a == 1) {
            // Coroutine name
            printf("%-16s ; name=K[%d]", name, bx);
            if (bx < PROTO_CONST_COUNT(proto)) {
                printf(" \"");
                xr_value_print(PROTO_CONSTANT(proto, bx));
                printf("\"");
            }
            printf("\n");
        } else if (a == 2) {
            // Coroutine priority
            const char *prio = bx == 0 ? "LOW" : bx == 1 ? "NORMAL" : bx == 2 ? "HIGH" : "?";
            printf("%-16s ; priority=%s(%d)\n", name, prio, bx);
        } else if (a == 3) {
            // Link mode
            const char *mode = bx == 1 ? "LINKED" : bx == 2 ? "MONITORED" : "?";
            printf("%-16s ; link_mode=%s(%d)\n", name, mode, bx);
        } else {
            printf("%-16s\n", name);
        }
        return offset + 1;
    }

    // Other special instructions
    printf("%-16s\n", name);
    return offset + 1;
}

/* ========== Value Printing ========== */

// Print constants table
void xr_print_constants(XrProto *proto) {
    XR_DCHECK(proto != NULL, "xr_print_constants: NULL proto");
    int count = PROTO_CONST_COUNT(proto);
    if (count == 0) {
        return;
    }

    printf("Constants:\n");
    for (int i = 0; i < count; i++) {
        printf("  K[%d] = ", i);
        xr_value_print(PROTO_CONSTANT(proto, i));
        printf("\n");
    }
}

/* ========== Disassembly API ========== */

// Disassemble single instruction (table-driven)
int xr_disassemble_instruction(XrProto *proto, int offset) {
    XR_DCHECK(proto != NULL, "xr_disassemble_instruction: NULL proto");
    // Print offset
    printf("%04d ", offset);

    // Print line number
    int lineinfo_count = DYNARRAY_COUNT(&proto->lineinfo);
    if (offset > 0 && offset < lineinfo_count &&
        PROTO_LINE(proto, offset) == PROTO_LINE(proto, offset - 1)) {
        printf("   | ");
    } else if (offset < lineinfo_count) {
        printf("%4d ", PROTO_LINE(proto, offset));
    } else {
        printf("   ? ");
    }

    // Get instruction
    XrInstruction inst = PROTO_CODE(proto, offset);
    OpCode op = GET_OPCODE(inst);

    // Table-driven: lookup metadata (centralised in runtime/value/xopcode_info.c).
    const XrOpCodeInfo *info = xr_opcode_info(op);
    if (op >= NUM_OPCODES || info->name == NULL || info->desc == NULL) {
        printf("UNKNOWN [opcode=%d]\n", op);
        return offset + 1;
    }

    // Dispatch by format
    switch (info->format) {
        case FMT_NONE:
            return disasm_none(info->name, offset);

        case FMT_A:
            return disasm_a(info->name, inst, offset);

        case FMT_AB:
            return disasm_ab(info->name, inst, offset);

        case FMT_ABC:
            return disasm_abc(info->name, inst, offset);

        case FMT_ABx:
            return disasm_abx(info->name, proto, inst, offset);

        case FMT_AsBx:
            return disasm_asbx(info->name, inst, offset);

        case FMT_sJ:
            return disasm_sj(info->name, inst, offset);

        case FMT_AB_sC:
            return disasm_ab_sc(info->name, inst, offset);

        case FMT_AsB_C:
            return disasm_asb_c(info->name, inst, offset);

        case FMT_AB_IMM:
            return disasm_ab_imm(info->name, inst, offset);

        case FMT_ABx_INT:
            return disasm_abx_int(info->name, inst, offset);

        case FMT_PROTO:
            return disasm_proto(info->name, proto, inst, offset);

        case FMT_GLOBAL:
            return disasm_abx_global(info->name, proto, inst, offset);

        case FMT_SPECIAL:
            return disasm_special(info->name, proto, inst, offset);

        default:
            printf("INVALID_FORMAT [op=%d, fmt=%d]\n", op, info->format);
            return offset + 1;
    }
}

// Disassemble entire function prototype
void xr_disassemble_proto(XrProto *proto, const char *name) {
    XR_DCHECK(proto != NULL, "xr_disassemble_proto: NULL proto");
    printf("== ");
    if (name != NULL) {
        printf("%s ", name);
    } else if (proto->name != NULL) {
        printf("%s ", proto->name->data);
    } else {
        printf("<script> ");
    }
    printf("==\n");

    // Print function info
    printf("Parameters: %d, Stack: %d, Code: %d", proto->numparams, proto->maxstacksize,
           PROTO_CODE_COUNT(proto));
    if (proto->return_type != NULL) {
        printf(", Returns: %s", proto->return_type);
    }
    printf("\n");

    // Print param_types coverage
    if (proto->param_types && proto->param_types_count > 0) {
        int typed = 0;
        for (int i = 0; i < proto->param_types_count; i++) {
            if (proto->param_types[i])
                typed++;
        }
        printf("ParamTypes: %d/%d typed", typed, proto->param_types_count);
        if (typed > 0) {
            printf(" [");
            for (int i = 0; i < proto->param_types_count; i++) {
                if (proto->param_types[i]) {
                    uint8_t tag = xr_type_to_slot_type(proto->param_types[i]);
                    const char *name = (tag == XR_SLOT_I64)    ? "i64"
                                       : (tag == XR_SLOT_F64)  ? "f64"
                                       : (tag == XR_SLOT_PTR)  ? "ptr"
                                       : (tag == XR_SLOT_BOOL) ? "bool"
                                                               : "unknown";
                    printf("R%d=%s ", i, name);
                }
            }
            printf("]");
        }
        printf("\n");
    }

    // Print constants table
    if (PROTO_CONST_COUNT(proto) > 0) {
        xr_print_constants(proto);
        printf("\n");
    }

    // Disassemble all instructions
    int code_count = PROTO_CODE_COUNT(proto);
    for (int offset = 0; offset < code_count;) {
        offset = xr_disassemble_instruction(proto, offset);
    }

    // Disassemble nested functions
    int proto_count = PROTO_PROTO_COUNT(proto);
    if (proto_count > 0) {
        printf("\nNested functions:\n");
        for (int i = 0; i < proto_count; i++) {
            XrProto *child = PROTO_PROTO(proto, i);
            if (child != NULL) {
                printf("\n");
                xr_disassemble_proto(child, NULL);
            } else {
                printf("\n[Null proto at index %d]\n", i);
            }
        }
    }
}
