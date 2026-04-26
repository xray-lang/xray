/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xpeephole.c - Xray Peephole optimizer implementation
 */

#include "xpeephole.h"
#include "../../base/xchecks.h"
#include "../../base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Optimization statistics
PeepholeStats g_peephole_stats = {0};

// ========== Helper Functions ==========

/*
 * Check if instruction is a jump
 */
bool xr_peep_is_jump(OpCode op) {
    return op == OP_JMP;
}

/*
 * Check if instruction has no side effects (can be safely deleted)
 */
bool xr_peep_no_side_effect(OpCode op) {
    switch (op) {
        case OP_MOVE:
        case OP_LOADI:
        case OP_LOADF:
        case OP_LOADK:
        case OP_LOADNULL:
        case OP_LOADTRUE:
        case OP_LOADFALSE:
        case OP_ADD:
        case OP_ADDI:
        case OP_ADDK:
        case OP_SUB:
        case OP_SUBI:
        case OP_SUBK:
        case OP_MUL:
        case OP_MULI:
        case OP_MULK:
        case OP_DIV:
        case OP_DIVK:
        case OP_MOD:
        case OP_MODK:
        case OP_UNM:
        case OP_NOT:
        case OP_NOP:
            return true;
        default:
            return false;
    }
}

/*
 * Find final target of jump (follow jump chain)
 */
int xr_peep_finaltarget(XrProto *proto, int pc, int size) {
    int target = pc;
    int count = 0;

    // Limit search depth to prevent infinite loop
    while (count < 100 && target >= 0 && target < size) {
        XrInstruction inst = PROTO_CODE(proto, target);
        OpCode op = GET_OPCODE(inst);

        if (op != OP_JMP) {
            break;  // Found final target
        }

        // Follow jump
        int offset = GETARG_sJ(inst);
        target = target + 1 + offset;
        count++;
    }

    return target;
}

// ========== Optimization Implementation ==========

/*
 * Jump chain elimination
 */
int xr_peep_jump_chain(XrProto *proto) {
    int opt_count = 0;

    for (int pc = 0; pc < PROTO_CODE_COUNT(proto); pc++) {
        XrInstruction inst = PROTO_CODE(proto, pc);
        OpCode op = GET_OPCODE(inst);

        if (op == OP_JMP) {
            // Calculate jump target
            int offset = GETARG_sJ(inst);
            int target = pc + 1 + offset;

            // Find final target
            int final = xr_peep_finaltarget(proto, target, PROTO_CODE_COUNT(proto));

            // If final target differs, update jump
            if (final != target && final >= 0 && final < PROTO_CODE_COUNT(proto)) {
                int new_offset = final - pc - 1;

                // Check if offset is within range
                if (new_offset >= -MAXARG_sJ && new_offset <= MAXARG_sJ) {
                    PROTO_CODE(proto, pc) = CREATE_sJ(OP_JMP, new_offset);
                    opt_count++;
                    g_peephole_stats.jump_chain_opt++;
                }
            }
        }
    }

    return opt_count;
}

/*
 * Check if instruction uses a register as source operand
 */
static bool uses_register_as_source(XrInstruction inst, int reg) {
    OpCode op = GET_OPCODE(inst);

    // ABC format: check B and C
    switch (op) {
        case OP_MOVE:
        case OP_NOT:
        case OP_UNM:
            // Only B is source
            return GETARG_B(inst) == reg;

        case OP_ADD:
        case OP_SUB:
        case OP_MUL:
        case OP_DIV:
        case OP_MOD:
            // Both B and C are sources
            return GETARG_B(inst) == reg || GETARG_C(inst) == reg;

        case OP_ADDI:
        case OP_SUBI:
        case OP_MULI:
            // Only B is source (C is immediate)
            return GETARG_B(inst) == reg;

        default:
            // Conservative: assume may be used
            return true;
    }
}

/*
 * Redundant instruction elimination
 *
 * Delete instructions that will be overwritten
 */
int xr_peep_redundant(XrProto *proto) {
    int opt_count = 0;

    for (int pc = 0; pc < PROTO_CODE_COUNT(proto) - 1; pc++) {
        XrInstruction inst1 = PROTO_CODE(proto, pc);
        XrInstruction inst2 = PROTO_CODE(proto, pc + 1);

        OpCode op1 = GET_OPCODE(inst1);
        OpCode op2 = GET_OPCODE(inst2);

        /* Eliminate redundant UNBOX+BOX / BOX+UNBOX pairs.
         * Pattern: UNBOX_F64 Ra Rb; BOX_F64 Rc Ra  (where Rc == Rb or Ra == Rb == Rc)
         * Most common: UNBOX_F64 R[x] R[x]; BOX_F64 R[x] R[x] → both NOP
         */
        if ((op1 == OP_UNBOX_F64 && op2 == OP_BOX_F64) ||
            (op1 == OP_UNBOX_I64 && op2 == OP_BOX_I64) ||
            (op1 == OP_BOX_F64 && op2 == OP_UNBOX_F64) ||
            (op1 == OP_BOX_I64 && op2 == OP_UNBOX_I64)) {
            int a1 = GETARG_A(inst1), b1 = GETARG_B(inst1);
            int a2 = GETARG_A(inst2), b2 = GETARG_B(inst2);

            /* Safe to eliminate when the second reads what the first wrote,
             * and the final result ends up in the same register as the original source.
             * Common case: both A==B on both instructions (in-place ops on same reg).
             */
            if (b2 == a1 && a2 == b1) {
                PROTO_CODE(proto, pc) = CREATE_ABC(OP_NOP, 0, 0, 0);
                PROTO_CODE(proto, pc + 1) = CREATE_ABC(OP_NOP, 0, 0, 0);
                opt_count += 2;
                g_peephole_stats.redundant_removed += 2;
                pc++;  // Skip the second instruction
                continue;
            }
        }

        // Detect pattern: consecutive instructions writing to same register
        // Skip NOP: its A field carries metadata, not a register target
        if (op1 != OP_NOP && op2 != OP_NOP && xr_peep_no_side_effect(op1) &&
            xr_peep_no_side_effect(op2)) {
            int a1 = GETARG_A(inst1);
            int a2 = GETARG_A(inst2);

            // If writing to same register, check if second depends on first's result
            if (a1 == a2) {
                // If second instruction uses first's result as source, cannot delete
                if (uses_register_as_source(inst2, a1)) {
                    continue;  // Cannot delete, skip
                }

                // Replace first instruction with NOP
                PROTO_CODE(proto, pc) = CREATE_ABC(OP_NOP, 0, 0, 0);
                opt_count++;
                g_peephole_stats.redundant_removed++;
            }
        }
    }

    return opt_count;
}

/*
 * Useless MOVE elimination
 */
int xr_peep_useless_move(XrProto *proto) {
    int opt_count = 0;

    for (int pc = 0; pc < PROTO_CODE_COUNT(proto); pc++) {
        XrInstruction inst = PROTO_CODE(proto, pc);
        OpCode op = GET_OPCODE(inst);

        if (op == OP_MOVE) {
            int a = GETARG_A(inst);
            int b = GETARG_B(inst);

            // MOVE R[A], R[A] is useless
            if (a == b) {
                PROTO_CODE(proto, pc) = CREATE_ABC(OP_NOP, 0, 0, 0);
                opt_count++;
                g_peephole_stats.useless_move_removed++;
            }
        }
    }

    return opt_count;
}

/*
 * NOP compression
 * Delete all NOP instructions and recalculate jump offsets
 */
int xr_peep_compress_nop(XrProto *proto) {
    if (!proto || PROTO_CODE_COUNT(proto) == 0) {
        return 0;
    }

    // Check if special instructions depend on subsequent NOP placeholders
    for (int i = 0; i < PROTO_CODE_COUNT(proto); i++) {
        XrInstruction inst = PROTO_CODE(proto, i);
        OpCode op = GET_OPCODE(inst);

        // TRY depends on subsequent NOP storing finally_offset
        if (op == OP_TRY) {
            return 0;
        }

        // Spawn instructions may have subsequent metadata NOPs:
        //   A=1: coroutine name, A=2: priority, A=3: link mode
        if (op == OP_GO || op == OP_SPAWN_CONT || op == OP_GO_INVOKE) {
            for (int j = i + 1; j < PROTO_CODE_COUNT(proto); j++) {
                XrInstruction next = PROTO_CODE(proto, j);
                if (GET_OPCODE(next) != OP_NOP)
                    break;
                int nop_a = GETARG_A(next);
                if (nop_a >= 1 && nop_a <= 3) {
                    return 0;  // Has spawn metadata NOP, skip compression
                }
                break;
            }
        }
    }

    // Count NOP instructions
    int nop_count = 0;
    for (int i = 0; i < PROTO_CODE_COUNT(proto); i++) {
        if (GET_OPCODE(PROTO_CODE(proto, i)) == OP_NOP) {
            nop_count++;
        }
    }

    // If no NOPs, return directly
    if (nop_count == 0) {
        return 0;
    }

    // Create PC mapping table: old_pc -> new_pc
    int *pc_map = (int *) xr_malloc(PROTO_CODE_COUNT(proto) * sizeof(int));
    if (!pc_map) {
        return 0;
    }

    // Create new dynamic arrays
    int new_size = PROTO_CODE_COUNT(proto) - nop_count;
    XrDynArray new_code_arr;
    XrDynArray new_lineinfo_arr;
    DYNARRAY_INIT(&new_code_arr, XrInstruction);
    DYNARRAY_INIT(&new_lineinfo_arr, int);

    if (!xr_dynarray_reserve(&new_code_arr, new_size) ||
        (DYNARRAY_COUNT(&proto->lineinfo) > 0 &&
         !xr_dynarray_reserve(&new_lineinfo_arr, new_size))) {
        xr_free(pc_map);
        DYNARRAY_FREE(&new_code_arr);
        DYNARRAY_FREE(&new_lineinfo_arr);
        return 0;
    }

    // Reverse map: new_pc -> old_pc (avoids O(n²) linear scan later)
    int *rev_map = (int *) xr_malloc(new_size * sizeof(int));
    if (!rev_map) {
        xr_free(pc_map);
        DYNARRAY_FREE(&new_code_arr);
        DYNARRAY_FREE(&new_lineinfo_arr);
        return 0;
    }

    // Copy non-NOP instructions and build both mappings
    int new_pc = 0;
    for (int old_pc = 0; old_pc < PROTO_CODE_COUNT(proto); old_pc++) {
        pc_map[old_pc] = new_pc;

        XrInstruction inst = PROTO_CODE(proto, old_pc);
        OpCode op = GET_OPCODE(inst);

        if (op != OP_NOP) {
            rev_map[new_pc] = old_pc;
            DYNARRAY_ADD(&new_code_arr, inst, XrInstruction);
            if (DYNARRAY_COUNT(&proto->lineinfo) > 0) {
                int line = PROTO_LINE(proto, old_pc);
                DYNARRAY_ADD(&new_lineinfo_arr, line, int);
            }
            new_pc++;
        }
    }

    // Update all jump instruction offsets
    for (int pc = 0; pc < new_size; pc++) {
        XrInstruction inst = DYNARRAY_GET(&new_code_arr, pc, XrInstruction);
        OpCode op = GET_OPCODE(inst);

        // Handle JMP instruction
        if (op == OP_JMP) {
            int old_offset = GETARG_sJ(inst);
            int orig_pc = rev_map[pc];
            {
                int old_target = orig_pc + 1 + old_offset;

                // Ensure target is within range
                if (old_target >= 0 && old_target < PROTO_CODE_COUNT(proto)) {
                    int new_target = pc_map[old_target];
                    int new_offset = new_target - pc - 1;

                    // Check if offset is within range
                    if (new_offset >= -MAXARG_sJ && new_offset <= MAXARG_sJ) {
                        XrInstruction new_inst = CREATE_sJ(OP_JMP, new_offset);
                        DYNARRAY_SET(&new_code_arr, pc, new_inst, XrInstruction);
                    }
                }
            }
        }

        // Handle TRY instruction: update catch_offset
        if (op == OP_TRY) {
            int old_catch = GETARG_Bx(inst);
            if (old_catch > 0 && old_catch < PROTO_CODE_COUNT(proto)) {
                int new_catch = pc_map[old_catch];
                XrInstruction new_inst = CREATE_ABx(OP_TRY, GETARG_A(inst), new_catch);
                DYNARRAY_SET(&new_code_arr, pc, new_inst, XrInstruction);
            }
        }
    }

    // Replace code in XrProto
    DYNARRAY_FREE(&proto->code);
    proto->code = new_code_arr;

    // Replace line info
    if (DYNARRAY_COUNT(&proto->lineinfo) > 0) {
        DYNARRAY_FREE(&proto->lineinfo);
        proto->lineinfo = new_lineinfo_arr;
    } else {
        DYNARRAY_FREE(&new_lineinfo_arr);
    }

    xr_free(pc_map);
    xr_free(rev_map);

    g_peephole_stats.nop_compressed += nop_count;
    return nop_count;
}

/*
 * Tail call detection: CALL Ra Rb Rc; RETURN/RETURN1 Ra → TAILCALL Ra Rb
 *
 * WHY THIS DESIGN:
 *   Conservative: only convert when CALL+RETURN is the very last instruction
 *   pair in the function. OP_TAILCALL only handles XrClosure (not class
 *   constructors, native functions, etc.), so we skip functions with upvalues
 *   (method closures may call class constructors via tail position).
 */
int xr_peep_tail_call(XrProto *proto) {
    int opt_count = 0;
    int code_count = PROTO_CODE_COUNT(proto);

    // Skip if function has upvalues (closures need proper frame teardown)
    if (PROTO_UPVAL_COUNT(proto) > 0)
        return 0;

    // Only check the last two instructions (most reliable tail position)
    if (code_count < 2)
        return 0;

    // Find last non-NOP instruction pair
    int last_pc = code_count - 1;
    while (last_pc > 0 && GET_OPCODE(PROTO_CODE(proto, last_pc)) == OP_NOP) {
        last_pc--;
    }
    int prev_pc = last_pc - 1;
    while (prev_pc >= 0 && GET_OPCODE(PROTO_CODE(proto, prev_pc)) == OP_NOP) {
        prev_pc--;
    }
    if (prev_pc < 0)
        return 0;

    XrInstruction inst1 = PROTO_CODE(proto, prev_pc);
    XrInstruction inst2 = PROTO_CODE(proto, last_pc);
    OpCode op1 = GET_OPCODE(inst1);
    OpCode op2 = GET_OPCODE(inst2);

    if (op1 != OP_CALL)
        return 0;

    int call_a = GETARG_A(inst1);
    int call_b = GETARG_B(inst1);

    // Pattern 1: CALL Ra Rb Rc; RETURN Ra N (return call result)
    if (op2 == OP_RETURN && GETARG_A(inst2) == call_a) {
        PROTO_CODE(proto, prev_pc) = CREATE_ABC(OP_TAILCALL, call_a, call_b, 0);
        PROTO_CODE(proto, last_pc) = CREATE_ABC(OP_NOP, 0, 0, 0);
        opt_count++;
        g_peephole_stats.tail_call_opt++;
    }
    // Pattern 2: CALL Ra Rb Rc; RETURN1 Ra (return1 single call result)
    else if (op2 == OP_RETURN1 && GETARG_A(inst2) == call_a) {
        PROTO_CODE(proto, prev_pc) = CREATE_ABC(OP_TAILCALL, call_a, call_b, 0);
        PROTO_CODE(proto, last_pc) = CREATE_ABC(OP_NOP, 0, 0, 0);
        opt_count++;
        g_peephole_stats.tail_call_opt++;
    }

    return opt_count;
}

/*
 * Main optimization function
 */
int xr_peephole_optimize(XrProto *proto) {
    if (!proto || PROTO_CODE_COUNT(proto) == 0) {
        return 0;
    }

    int total = 0;

    // Execute various optimizations (generate NOP)
    total += xr_peep_jump_chain(proto);
    total += xr_peep_redundant(proto);
    total += xr_peep_tail_call(proto);
    total += xr_peep_useless_move(proto);

    // NOP compression (delete all NOPs)
    total += xr_peep_compress_nop(proto);

    g_peephole_stats.total_optimizations = total;

    // Recursively optimize nested functions
    for (int i = 0; i < PROTO_PROTO_COUNT(proto); i++) {
        total += xr_peephole_optimize(PROTO_PROTO(proto, i));
    }

    return total;
}

// ========== Statistics ==========

void xr_peephole_reset_stats(void) {
    memset(&g_peephole_stats, 0, sizeof(PeepholeStats));
}

void xr_peephole_print_stats(void) {
    if (g_peephole_stats.total_optimizations > 0) {
        printf("\n=== Peephole Optimization Stats ===\n");
        printf("Jump chain optimized: %d\n", g_peephole_stats.jump_chain_opt);
        printf("Redundant removed: %d\n", g_peephole_stats.redundant_removed);
        printf("Useless MOVE removed: %d\n", g_peephole_stats.useless_move_removed);
        printf("NOP compressed: %d\n", g_peephole_stats.nop_compressed);
        printf("Total optimizations: %d\n", g_peephole_stats.total_optimizations);
        printf("===================================\n");
    }
}
