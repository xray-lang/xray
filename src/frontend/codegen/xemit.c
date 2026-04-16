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
#include "xregalloc.h"
#include "../../base/xmalloc.h"
#include "../../base/xchecks.h"
#include "../../base/xlog.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    
    return pc;
}

int emit_abx(XrEmitter *e, OpCode op, int a, int bx) {
    XrInstruction inst = CREATE_ABx(op, a, bx);
    int pc = current_pc(e);
    
    // Peephole optimization
    if (e->enable_peephole && try_optimize_sequence(e, inst)) {
        e->stats.optimized_count++;
        return pc;
    }
    
    emitter_write_instruction(e, inst);
    emitter_update_window(e, inst, pc);
    
    return pc;
}

int emit_asbx(XrEmitter *e, OpCode op, int a, int sbx) {
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
    
    return emit_abc(e, OP_MOVE, dest, src, 0);
}

int emit_loadk(XrEmitter *e, int dest, int const_idx) {
    return emit_abx(e, OP_LOADK, dest, const_idx);
}

int emit_loadnull(XrEmitter *e, int reg) {
    return emit_abc(e, OP_LOADNULL, reg, 0, 0);
}

int emit_loadtrue(XrEmitter *e, int reg) {
    return emit_abc(e, OP_LOADTRUE, reg, 0, 0);
}

int emit_loadfalse(XrEmitter *e, int reg) {
    return emit_abc(e, OP_LOADFALSE, reg, 0, 0);
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
    return emit_abc(e, OP_CALL, func, nargs, nresults);
}

int emit_return(XrEmitter *e, int base, int nret) {
    // Flush Peephole window (no instructions after RETURN)
    emitter_flush_peephole(e);
    
    // Use optimized return instructions for common cases
    if (nret == 0) {
        return emit_abc(e, OP_RETURN0, 0, 0, 0);
    } else if (nret == 1) {
        return emit_abc(e, OP_RETURN1, base, 0, 0);
    }
    return emit_abc(e, OP_RETURN, base, nret, 0);
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
    return emit_abc(e, OP_TEST, cond_reg, 0, k);
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
    return emit_abc(e, OP_SPILL, slot, reg, 0);
}

/*
** Emit RELOAD instruction
** R[reg] = S[slot]
*/
int emit_reload(XrEmitter *e, int reg, int slot) {
    XR_CHECK(e != NULL, "Emitter cannot be NULL");
    return emit_abc(e, OP_RELOAD, reg, slot, 0);
}

