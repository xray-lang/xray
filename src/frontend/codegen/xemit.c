/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xemit.c - Instruction emitter implementation
 */

#include "xemit.h"
#include "xcompiler_context.h"
#include "xcompiler_emit.h"  // xr_codegen_record_gc_safepoint
#include "xregalloc.h"
#include "../../base/xmalloc.h"
#include "../../base/xchecks.h"
#include "../../base/xlog.h"
#include "../../runtime/value/xopcode_info.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Bitmap of opcodes that allocate GC-managed objects (safepoints for precise GC).
static inline bool opcode_is_gc_alloc(OpCode op) {
    switch (op) {
        case OP_NEWARRAY:
        case OP_NEWMAP:
        case OP_NEWSET:
        case OP_NEWJSON:
        case OP_NEWSTRINGBUILDER:
        case OP_NEWRANGE:
        case OP_CLOSURE:
        case OP_CELL_NEW:
            return true;
        default:
            return false;
    }
}

// Validate one operand byte against its declared field-kind contract.
// Runs only in debug builds (XR_DCHECK_FMT compiles to no-op in release).
// The kind triple is published by xopcode_def.h and the validation here
// is the emitter side of that contract — fields tagged NONE must be
// zero, K_IDX must be in range of the constants table, etc.
//
// We *cannot* distinguish REG_IN from REG_OUT at this layer (both
// arrive as int register indices), so the IN/OUT direction is enforced
// upstream by the strongly typed emitter wrappers (planned). What we
// *can* enforce here is that no slot tagged NONE accidentally gets a
// non-zero byte — that's the failure mode behind the OP_DUMP regression
// where slot C should have been zero but landed as a stale register
// index, silently making OP_DUMP read the wrong register.
static void validate_field(XrEmitter *e, OpCode op, char slot,
                           XrOpFieldKind kind, int value) {
    XR_DCHECK(e != NULL && e->proto != NULL,
              "validate_field: emitter/proto NULL");
    const char *opname = xr_opcode_name(op);
    switch (kind) {
    case XR_OPF_NONE:
        XR_DCHECK_FMT(value == 0,
            "emit %s: slot %c declared UNUSED but received %d "
            "(must be 0; likely a swapped operand)",
            opname, slot, value);
        break;
    case XR_OPF_REG_OUT:
    case XR_OPF_REG_IN:
    case XR_OPF_REG_INOUT:
    case XR_OPF_REG_BASE:
        XR_DCHECK_FMT(value >= 0 && value <= MAXARG_A,
            "emit %s: slot %c expected register, got %d (out of 0..%d)",
            opname, slot, value, MAXARG_A);
        break;
    case XR_OPF_LIT:
        XR_DCHECK_FMT(value >= 0 && value <= MAXARG_B,
            "emit %s: slot %c literal out of 0..%d, got %d",
            opname, slot, MAXARG_B, value);
        break;
    case XR_OPF_LIT_S:
        XR_DCHECK_FMT(value >= -128 && value <= 127,
            "emit %s: slot %c signed literal out of -128..127, got %d",
            opname, slot, value);
        break;
    case XR_OPF_LIT_FLAG:
        XR_DCHECK_FMT(value == 0 || value == 1,
            "emit %s: slot %c flag must be 0 or 1, got %d",
            opname, slot, value);
        break;
    case XR_OPF_K_IDX:
        XR_DCHECK_FMT(value >= 0 && value < PROTO_CONST_COUNT(e->proto),
            "emit %s: slot %c K-index %d out of range (0..%d)",
            opname, slot, value, PROTO_CONST_COUNT(e->proto) - 1);
        break;
    case XR_OPF_SYMBOL_IDX:
        XR_DCHECK_FMT(value >= 0 && value < PROTO_SYMBOL_COUNT(e->proto),
            "emit %s: slot %c symbol index %d out of range (0..%d) "
            "— call emitter_add_symbol() before emitting",
            opname, slot, value, PROTO_SYMBOL_COUNT(e->proto) - 1);
        break;
    case XR_OPF_PROTO_IDX:
        XR_DCHECK_FMT(value >= 0 && value < PROTO_PROTO_COUNT(e->proto),
            "emit %s: slot %c sub-proto index %d out of range (0..%d)",
            opname, slot, value, PROTO_PROTO_COUNT(e->proto) - 1);
        break;
    case XR_OPF_GLOBAL_IDX:
    case XR_OPF_BUILTIN_IDX:
        XR_DCHECK_FMT(value >= 0,
            "emit %s: slot %c global/builtin index must be >= 0, got %d",
            opname, slot, value);
        break;
    case XR_OPF_JUMP:
    case XR_OPF_SUB_OPCODE:
    case XR_OPF_SPECIAL:
        // No standard constraint; emitter-internal checks live elsewhere.
        break;
    }
}

static void validate_abc(XrEmitter *e, OpCode op, int a, int b, int c) {
    const XrOpCodeInfo *info = xr_opcode_info(op);
    validate_field(e, op, 'A', info->field_kind[0], a);
    validate_field(e, op, 'B', info->field_kind[1], b);
    validate_field(e, op, 'C', info->field_kind[2], c);
}

static void validate_abx(XrEmitter *e, OpCode op, int a, int bx) {
    const XrOpCodeInfo *info = xr_opcode_info(op);
    validate_field(e, op, 'A', info->field_kind[0], a);
    // For ABx formats the B slot in field_kind describes the full Bx.
    XrOpFieldKind kind = info->field_kind[1];
    const char *opname = xr_opcode_name(op);
    switch (kind) {
    case XR_OPF_K_IDX:
        XR_DCHECK_FMT(bx >= 0 && bx < PROTO_CONST_COUNT(e->proto),
            "emit %s: Bx K-index %d out of range (0..%d)",
            opname, bx, PROTO_CONST_COUNT(e->proto) - 1);
        break;
    case XR_OPF_PROTO_IDX:
        XR_DCHECK_FMT(bx >= 0 && bx < PROTO_PROTO_COUNT(e->proto),
            "emit %s: Bx proto index %d out of range (0..%d)",
            opname, bx, PROTO_PROTO_COUNT(e->proto) - 1);
        break;
    case XR_OPF_GLOBAL_IDX:
    case XR_OPF_BUILTIN_IDX:
        XR_DCHECK_FMT(bx >= 0,
            "emit %s: Bx global/builtin index must be >= 0, got %d",
            opname, bx);
        break;
    case XR_OPF_LIT:
        XR_DCHECK_FMT(bx >= 0 && bx <= MAXARG_Bx,
            "emit %s: Bx literal out of 0..%d, got %d",
            opname, MAXARG_Bx, bx);
        break;
    case XR_OPF_LIT_S:
    case XR_OPF_SPECIAL:
        // Bx may be a pre-encoded signed value or composite payload;
        // emitter cannot meaningfully range-check it here.
        break;
    default:
        XR_DCHECK_FMT(bx >= 0 && bx <= MAXARG_Bx,
            "emit %s: Bx out of 0..%d, got %d",
            opname, MAXARG_Bx, bx);
        break;
    }
}

static void validate_asbx(XrEmitter *e, OpCode op, int a, int sbx) {
    const XrOpCodeInfo *info = xr_opcode_info(op);
    validate_field(e, op, 'A', info->field_kind[0], a);
    XR_DCHECK_FMT(sbx >= LOADI_MIN && sbx <= LOADI_MAX,
        "emit %s: sBx out of %d..%d, got %d",
        xr_opcode_name(op), LOADI_MIN, LOADI_MAX, sbx);
}

static inline int current_pc(XrEmitter *e) {
    return PROTO_CODE_COUNT(e->proto);
}

/*
** Write instruction to XrProto
** @param inst Instruction to write
**
** Automatically gets source line number from ctx->current_line
*/
void emitter_write_instruction(XrEmitter *e, XrInstruction inst) {
    XR_DCHECK(e != NULL, "emitter_write: NULL emitter");
    XR_DCHECK(e->proto != NULL, "emitter_write: NULL proto");
    int line = e->ctx->current_line;
    xr_vm_proto_write(e->proto, inst, line);
    e->pc = current_pc(e);
    e->stats.inst_count++;

    // LIFO mode does not require PC tracking

    // Debug mode: print instruction
    if (e->debug_mode) {
        printf("[Emit] PC=%d, Line=%d, Inst=0x%08x, OP=%d\n",
               e->pc - 1, line, inst, GET_OPCODE(inst));
    }
}

void emitter_update_window(XrEmitter *e, XrInstruction inst, int pc) {
    e->window.last_inst = inst;
    e->window.last_pc = pc;
    e->window.last_op = GET_OPCODE(inst);
    e->window.last_dest = GETARG_A(inst);  // For most instructions, A is the destination
    e->window.can_optimize = true;
}

XrEmitter* emitter_new(XrCompilerContext *ctx, XrProto *proto, XRegAlloc *regalloc) {
    XR_DCHECK(ctx != NULL, "emitter_new: NULL ctx");
    XR_DCHECK(proto != NULL, "emitter_new: NULL proto");
    XrEmitter *e = (XrEmitter*)xr_malloc(sizeof(XrEmitter));
    if (!e) {
        xr_log_warning("emitter", "failed to allocate emitter");
        return NULL;
    }

    memset(e, 0, sizeof(XrEmitter));

    e->ctx = ctx;               // Save context for getting line number
    e->proto = proto;
    e->regalloc = regalloc;
    e->pc = current_pc(e);
    e->enable_peephole = true;  // Enable optimization (redundant MOVE removal)
    e->debug_mode = false;

    // Initialize Peephole window
    e->window.can_optimize = false;
    e->window.last_inst = 0;
    e->window.last_pc = -1;
    e->window.last_op = OP_NOP;
    e->window.last_dest = -1;

    return e;
}

void emitter_free(XrEmitter *e) {
    if (e) {
        xr_free(e);
    }
}

void emitter_set_peephole(XrEmitter *e, bool enable) {
    XR_DCHECK(e != NULL, "emitter_set_peephole: NULL emitter");
    e->enable_peephole = enable;
    if (!enable) {
        emitter_flush_peephole(e);
    }
}

void emitter_set_debug(XrEmitter *e, bool enable) {
    XR_DCHECK(e != NULL, "emitter_set_debug: NULL emitter");
    e->debug_mode = enable;
}

/* ========== Basic Instruction Emission ========== */

int emit_abc(XrEmitter *e, OpCode op, int a, int b, int c) {
    XR_DCHECK(a >= 0, "emit_abc: negative register A");
#if XR_DEBUG
    validate_abc(e, op, a, b, c);
#endif
    XrInstruction inst = CREATE_ABC(op, a, b, c);
    int pc = current_pc(e);

    // Peephole optimization
    if (e->enable_peephole && try_optimize_sequence(e, inst)) {
        e->stats.optimized_count++;
        return pc;  // Optimized out, do not emit
    }

    // Normal emission
    emitter_write_instruction(e, inst);
    emitter_update_window(e, inst, pc);

    // Statistics
    if (op == OP_MOVE) {
        e->stats.move_count++;
    }

    // Record GC safepoint for allocation instructions
    if (opcode_is_gc_alloc(op) && e->ctx && e->ctx->current) {
        xr_codegen_record_gc_safepoint(e->ctx->current);
    }

    return pc;
}

int emit_abx(XrEmitter *e, OpCode op, int a, int bx) {
#if XR_DEBUG
    validate_abx(e, op, a, bx);
#endif
    XrInstruction inst = CREATE_ABx(op, a, bx);
    int pc = current_pc(e);

    // Peephole optimization
    if (e->enable_peephole && try_optimize_sequence(e, inst)) {
        e->stats.optimized_count++;
        return pc;
    }

    emitter_write_instruction(e, inst);
    emitter_update_window(e, inst, pc);

    // Record GC safepoint for allocation instructions (OP_NEWJSON uses ABx format)
    if (opcode_is_gc_alloc(op) && e->ctx && e->ctx->current) {
        xr_codegen_record_gc_safepoint(e->ctx->current);
    }

    return pc;
}

int emit_asbx(XrEmitter *e, OpCode op, int a, int sbx) {
#if XR_DEBUG
    validate_asbx(e, op, a, sbx);
#endif
    XrInstruction inst = CREATE_AsBx(op, a, sbx);
    int pc = current_pc(e);

    emitter_write_instruction(e, inst);
    emitter_update_window(e, inst, pc);

    return pc;
}

int emit_sj(XrEmitter *e, OpCode op, int sj) {
    XrInstruction inst = CREATE_sJ(op, sj);
    int pc = current_pc(e);

    emitter_write_instruction(e, inst);
    e->stats.jump_count++;

    return pc;
}

/* ========== High-Level Emission API ========== */

int emit_move(XrEmitter *e, int dest, int src) {
    // Optimization: dest == src, skip
    if (dest == src) {
        if (e->debug_mode) {
            printf("[Emit] Skip redundant MOVE R[%d], R[%d]\n", dest, src);
        }
        e->stats.optimized_count++;
        return current_pc(e);
    }

    return xemit_move(e, dest, src);
}

int emit_loadk(XrEmitter *e, int dest, int const_idx) {
    return xemit_loadk(e, dest, const_idx);
}

int emit_loadnull(XrEmitter *e, int reg) {
    return xemit_loadnull(e, reg);
}

int emit_loadtrue(XrEmitter *e, int reg) {
    return xemit_loadtrue(e, reg);
}

int emit_loadfalse(XrEmitter *e, int reg) {
    return xemit_loadfalse(e, reg);
}

int emit_binary_op(XrEmitter *e, OpCode op, int dest, int lhs, int rhs) {
    return emit_abc(e, op, dest, lhs, rhs);
}

int emit_unary_op(XrEmitter *e, OpCode op, int dest, int src) {
    return emit_abc(e, op, dest, src, 0);
}

int emitter_add_symbol(XrEmitter *e, int global_symbol) {
    return xr_proto_add_symbol(e->proto, global_symbol);
}

int emit_call(XrEmitter *e, int func, int nargs, int nresults) {
    return xemit_call(e, func, nargs, nresults);
}

int emit_return(XrEmitter *e, int base, int nret) {
    // Flush Peephole window (no instructions after RETURN)
    emitter_flush_peephole(e);

    // Use optimized return instructions for common cases
    if (nret == 0) {
        return xemit_return0(e);
    } else if (nret == 1) {
        return xemit_return1(e, base);
    }
    return xemit_return(e, base, nret);
}

/* ========== Jump Instructions ========== */

int emit_jump(XrEmitter *e, OpCode op) {
    // Reset peephole window: JMP creates control flow boundary
    e->window.can_optimize = false;
    e->window.last_op = OP_NOP;

    int pc = current_pc(e);
    XrInstruction inst = CREATE_sJ(op, 0);
    emitter_write_instruction(e, inst);
    e->stats.jump_count++;
    return pc;
}

void patch_jump(XrEmitter *e, int jump_pc, int target_pc) {
    XR_DCHECK(jump_pc >= 0 && jump_pc < current_pc(e), "patch_jump: jump_pc out of range");
    if (target_pc < 0) {
        target_pc = current_pc(e);
    }

    int jump = target_pc - jump_pc - 1;

    if (jump > MAXARG_sJ || jump < -MAXARG_sJ) {
        xr_log_warning("emitter", "jump offset too large: %d", jump);
        return;
    }

    XrInstruction *inst = PROTO_CODE_PTR(e->proto, jump_pc);
    OpCode op = GET_OPCODE(*inst);
    *inst = CREATE_sJ(op, jump);

    if (e->debug_mode) {
        printf("[Emit] Patch jump at PC=%d, offset=%d, target=%d\n",
               jump_pc, jump, target_pc);
    }
}

static int get_jump_list_next(XrEmitter *e, int pc) {
    if (pc == NO_JUMP) {
        return NO_JUMP;
    }

    XrInstruction *inst = PROTO_CODE_PTR(e->proto, pc);
    int offset = GETARG_sJ(*inst);

    if (offset == 0) {
        return NO_JUMP;
    }

    return pc + 1 + offset;
}

static void set_jump_list_next(XrEmitter *e, int pc, int next_pc) {
    if (pc == NO_JUMP) {
        return;
    }

    XrInstruction *inst = PROTO_CODE_PTR(e->proto, pc);
    OpCode op = GET_OPCODE(*inst);

    if (next_pc == NO_JUMP) {
        *inst = CREATE_sJ(op, 0);
    } else {
        int offset = next_pc - pc - 1;
        *inst = CREATE_sJ(op, offset);
    }
}

int jump_list_concat(XrEmitter *e, int list1, int list2) {
    if (list2 == NO_JUMP) {
        return list1;
    }
    if (list1 == NO_JUMP) {
        return list2;
    }

    int last = list1;
    int next = get_jump_list_next(e, last);

    while (next != NO_JUMP) {
        last = next;
        next = get_jump_list_next(e, last);
    }

    set_jump_list_next(e, last, list2);

    if (e->debug_mode) {
        printf("[Emit] Concat jump lists: list1=%d, list2=%d\n", list1, list2);
    }

    return list1;
}

/*
** Patch entire jump list
*/
void patch_jump_list(XrEmitter *e, int list, int target_pc) {
    if (list == NO_JUMP) {
        return;
    }

    // If target_pc < 0, use current PC
    if (target_pc < 0) {
        target_pc = current_pc(e);
    }

    // Iterate through list and patch all jumps
    int current = list;
    while (current != NO_JUMP) {
        int next = get_jump_list_next(e, current);

        // Patch current jump
        int jump = target_pc - current - 1;

        if (jump > MAXARG_sJ || jump < -MAXARG_sJ) {
            xr_log_warning("emitter", "jump offset too large: %d at PC=%d",
                    jump, current);
        } else {
            XrInstruction *inst = PROTO_CODE_PTR(e->proto, current);
            OpCode op = GET_OPCODE(*inst);
            *inst = CREATE_sJ(op, jump);

            if (e->debug_mode) {
                printf("[Emit] Patch jump list node at PC=%d, offset=%d, target=%d\n",
                       current, jump, target_pc);
            }
        }

        current = next;
    }
}

void emit_loop(XrEmitter *e, int loop_start) {
    int offset = current_pc(e) - loop_start + 1;

    if (offset > MAXARG_sJ) {
        xr_log_warning("emitter", "loop body too large: %d", offset);
        return;
    }

    // Reset peephole window: backward jump creates control flow boundary
    e->window.can_optimize = false;
    e->window.last_op = OP_NOP;

    // Emit backward jump directly in sJ format
    XrInstruction inst = CREATE_sJ(OP_JMP, -offset);
    emitter_write_instruction(e, inst);
    e->stats.jump_count++;
}

int emit_test(XrEmitter *e, int cond_reg, bool negate) {
    // TEST instruction: if (R[A]) != k then PC++
    // k in C field: 0 = false, 1 = true
    int k = negate ? 1 : 0;
    return xemit_test(e, cond_reg, k);
}

/* ========== Debugging and Statistics ========== */

void emitter_print_stats(XrEmitter *e) {
    printf("\n========== XrEmitter Statistics ==========\n");
    printf("Total instructions:      %d\n", e->stats.inst_count);
    printf("MOVE instructions:       %d\n", e->stats.move_count);
    printf("Jump instructions:       %d\n", e->stats.jump_count);
    printf("Optimized out:           %d\n", e->stats.optimized_count);
    printf("Optimization rate:       %.2f%%\n",
           e->stats.inst_count > 0
           ? (e->stats.optimized_count * 100.0 / (e->stats.inst_count + e->stats.optimized_count))
           : 0.0);
    printf("========================================\n\n");
}

void emitter_flush_peephole(XrEmitter *e) {
    e->window.can_optimize = false;
    e->window.last_inst = 0;
    e->window.last_pc = -1;
    e->window.last_op = OP_NOP;
    e->window.last_dest = -1;
}

bool try_optimize_sequence(XrEmitter *e, XrInstruction inst) {
    if (!e->window.can_optimize) {
        return false;
    }

    if (optimize_redundant_move(e, inst)) return true;

    return false;
}

/* ========== Relocatable Expression Support ========== */

/*
** Modify A field of generated instruction (target register).
** This is the core mechanism for implementing relocatable expressions.
**
** WHY THIS DESIGN:
**   A field is always at bits 8-15 in all instruction formats (ABC, ABx, AsBx).
**   Direct bit manipulation is both simpler and correct for all formats,
**   avoiding a fragile opcode-to-format lookup table.
*/
void emit_patch_instruction_A(XrEmitter *e, int pc, int new_A) {
    XR_CHECK(e != NULL, "Emitter cannot be NULL");
    XR_CHECK(e->proto != NULL, "XrProto cannot be NULL");
    XR_CHECK(pc >= 0 && pc < PROTO_CODE_COUNT(e->proto), "Invalid PC for patch");
    XR_CHECK(new_A >= 0 && new_A < 256, "Invalid register A");

    XrInstruction *inst = PROTO_CODE_PTR(e->proto, pc);
    int old_a XR_UNUSED = GETARG_A(*inst);

    // Clear A field (bits 8-15) and set new value
    *inst = (*inst & ~((XrInstruction)0xFFu << 8)) | ((XrInstruction)(new_A & 0xFF) << 8);

    if (e->debug_mode) {
        printf("[Emitter] Patched PC=%d: A=%d->%d (op=%d)\n",
               pc, old_a, new_A, GET_OPCODE(*inst));
    }
}

/*
** Get generated instruction (for write-back modification)
*/
XrInstruction* emit_get_instruction(XrEmitter *e, int pc) {
    XR_CHECK(e != NULL, "Emitter cannot be NULL");
    XR_CHECK(e->proto != NULL, "XrProto cannot be NULL");
    XR_CHECK(pc >= 0 && pc < PROTO_CODE_COUNT(e->proto), "Invalid PC");

    return PROTO_CODE_PTR(e->proto, pc);
}

/*
** Get current PC (position of next instruction)
*/
int emit_get_current_pc(XrEmitter *e) {
    XR_CHECK(e != NULL, "Emitter cannot be NULL");
    XR_CHECK(e->proto != NULL, "XrProto cannot be NULL");

    return PROTO_CODE_COUNT(e->proto);
}

/* ========== Spill Support Implementation ========== */

/*
** Emit SPILL instruction
** S[slot] = R[reg]
*/
int emit_spill(XrEmitter *e, int slot, int reg) {
    XR_CHECK(e != NULL, "Emitter cannot be NULL");
    return xemit_spill(e, slot, reg);
}

/*
** Emit RELOAD instruction
** R[reg] = S[slot]
*/
int emit_reload(XrEmitter *e, int reg, int slot) {
    XR_CHECK(e != NULL, "Emitter cannot be NULL");
    return xemit_reload(e, reg, slot);
}

