/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xfusion.c - Instruction fusion optimizer implementation
 */

#include "xfusion.h"
#include "../../base/xchecks.h"
#include "../../runtime/value/xvalue.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Optimization statistics
FusionStats g_fusion_stats = {0};

/* ========== Helper Functions ========== */

// Check if XrValue is a small integer
bool xr_fusion_is_small_int(XrValue value, int *imm) {
    if (XR_IS_INT(value)) {
        xr_Integer val = XR_TO_INT(value);
        // Check if within signed byte range (-128 to 127)
        if (val >= -128 && val <= 127) {
            *imm = (int) val;
            return true;
        }
    }
    // Float constants must NOT be folded into ADDI/SUBI/MULI:
    // losing float type causes int+float to produce int instead of float
    return false;
}

// Check if constant is 0, 1, or -1 (most common constants)
static bool xr_fusion_is_common_const(XrValue value, int *imm) {
    if (XR_IS_INT(value)) {
        xr_Integer val = XR_TO_INT(value);
        if (val == 0 || val == 1 || val == -1) {
            *imm = (int) val;
            return true;
        }
    }
    // Float constants must NOT be fused: LOADI produces XR_TAG_I64, not XR_TAG_F64
    return false;
}

/* ========== Optimization Implementations ========== */

/*
** LOADK constant optimization
** Replace LOADK K(0/1/-1) with LOADI
**
** Note: This optimization is DISABLED (2025-10-16)
** Reason: Performance testing found LOADI is 5-10% slower than LOADK
** - LOADK accesses pre-allocated constant array (cache-friendly)
** - LOADI needs to call xr_int() to construct XrValue (function call overhead)
** - In fib(35) test, difference is about 50-100ms
**
** TODO: If xr_int() can be force-inlined, this can be re-enabled
*/
int xr_fusion_loadk_const(XrProto *proto) {
    XR_DCHECK(proto != NULL, "fusion_loadk_const: NULL proto");
// Re-enabled: xr_int() is now static inline, no function call overhead
#if 1
    int opt_count = 0;

    for (int pc = 0; pc < PROTO_CODE_COUNT(proto); pc++) {
        XrInstruction inst = PROTO_CODE(proto, pc);
        OpCode op = GET_OPCODE(inst);

        if (op == OP_LOADK) {
            int a = GETARG_A(inst);
            int bx = GETARG_Bx(inst);

            // Check constant value
            if (bx < PROTO_CONST_COUNT(proto)) {
                XrValue kval = PROTO_CONSTANT(proto, bx);
                int imm;

                // If it's a common constant, use LOADI instead
                if (xr_fusion_is_common_const(kval, &imm)) {
                    // Replace LOADK with LOADI
                    PROTO_CODE(proto, pc) = CREATE_AsBx(OP_LOADI, a, imm);
                    opt_count++;
                    g_fusion_stats.loadk_to_loadi++;
                }
            }
        }
    }

    return opt_count;
#endif
}

/*
** Arithmetic immediate optimization
** Recognize LOADK followed by arithmetic operation pattern and optimize
**
** Fix notes:
** 1. Immediate encoding fix: int8_t can be stored directly, no conversion needed
** 2. Register check: ensure LOADK's target register is only used once by following instruction
*/
int xr_fusion_arith_imm(XrProto *proto) {
    XR_DCHECK(proto != NULL, "fusion_arith_imm: NULL proto");
    int opt_count = 0;

    for (int pc = 0; pc < PROTO_CODE_COUNT(proto) - 1; pc++) {
        XrInstruction inst1 = PROTO_CODE(proto, pc);
        XrInstruction inst2 = PROTO_CODE(proto, pc + 1);

        OpCode op1 = GET_OPCODE(inst1);
        OpCode op2 = GET_OPCODE(inst2);

        // Pattern: LOADK Rx, K(n) + ADD/SUB/MUL Ry, Rz, Rx
        if (op1 == OP_LOADK) {
            int reg_k = GETARG_A(inst1);  // LOADK's target register
            int bx = GETARG_Bx(inst1);

            // Check if next instruction uses this register as second operand
            if (op2 == OP_ADD || op2 == OP_SUB || op2 == OP_MUL) {
                int a = GETARG_A(inst2);
                int b = GETARG_B(inst2);
                int c = GETARG_C(inst2);

                /* Ensure:
                 * 1. LOADK's result is only used by next instruction (c == reg_k)
                 * 2. LOADK's result is not the target register (avoid overwrite issue)
                 * 3. Constant is within valid range
                 */
                if (c == reg_k && a != reg_k && bx < PROTO_CONST_COUNT(proto)) {
                    XrValue kval = PROTO_CONSTANT(proto, bx);
                    int imm;

                    // If it's a small integer, can use immediate instruction
                    if (xr_fusion_is_small_int(kval, &imm)) {
                        /* Fix: Check left operand is not a string
                         * Problem: string + int constant would be incorrectly optimized to ADDI
                         * Solution: Backtrack to check R[b]'s source, ensure it's not a string
                         * constant
                         */
                        bool is_safe = true;

                        // Backtrack to check left operand R[b]'s source
                        for (int i = pc - 1; i >= 0 && i >= pc - 10; i--) {
                            XrInstruction prev = PROTO_CODE(proto, i);
                            OpCode prev_op = GET_OPCODE(prev);
                            int prev_a = GETARG_A(prev);

                            // Check if it's an assignment to R[b]
                            if (prev_op == OP_LOADK && prev_a == b) {
                                int prev_bx = GETARG_Bx(prev);
                                if (prev_bx < PROTO_CONST_COUNT(proto)) {
                                    XrValue left_val = PROTO_CONSTANT(proto, prev_bx);
                                    // If left operand is string, cannot use integer immediate
                                    // optimization
                                    if (XR_IS_STRING(left_val)) {
                                        is_safe = false;
                                        break;
                                    }
                                }
                                break;  // Found assignment source, stop backtracking
                            }

                            // If encountered other modification to R[b], stop backtracking
                            if (prev_a == b) {
                                break;
                            }
                        }

                        if (!is_safe) {
                            continue;  // Skip this optimization
                        }

                        // Select corresponding immediate instruction
                        OpCode new_op = OP_NOP;
                        switch (op2) {
                            case OP_ADD:
                                new_op = OP_ADDI;
                                break;
                            case OP_SUB:
                                new_op = OP_SUBI;
                                break;
                            case OP_MUL:
                                new_op = OP_MULI;
                                break;
                            default:
                                break;
                        }

                        if (new_op != OP_NOP) {
                            // Replace LOADK with NOP
                            PROTO_CODE(proto, pc) = CREATE_ABC(OP_NOP, 0, 0, 0);
                            // Replace arithmetic instruction with immediate form
                            // Fix: pass imm directly, int8_t will encode correctly
                            PROTO_SET_CODE(proto, pc + 1, CREATE_ABC(new_op, a, b, imm));
                            opt_count++;
                            g_fusion_stats.arith_to_imm++;
                        }
                    }
                }
            }
        }
    }

    return opt_count;
}

/*
** Comparison constant optimization
** Optimize LOADK in comparisons to immediate comparison
**
** Fix notes:
** 1. Fix immediate encoding bug: pass imm directly, no uint8_t conversion needed
** 2. Add register safety check
** 3. Correctly handle comparison instruction semantics (R[A] < sB, immediate in second operand
*position)
*/
int xr_fusion_cmp_const(XrProto *proto) {
    int opt_count = 0;

    for (int pc = 0; pc < PROTO_CODE_COUNT(proto) - 1; pc++) {
        XrInstruction inst1 = PROTO_CODE(proto, pc);
        XrInstruction inst2 = PROTO_CODE(proto, pc + 1);

        OpCode op1 = GET_OPCODE(inst1);
        OpCode op2 = GET_OPCODE(inst2);

        /* Pattern: LOADK Rx, K(n) + LT/LE/GT/GE Ra, Rx, k
         * Note: Immediate comparison instruction format is LTI Ra, sB, k (Ra < sB)
         * So we need to check if B parameter (second operand) is LOADK's target
         */
        if (op1 == OP_LOADK) {
            int reg_k = GETARG_A(inst1);
            int bx = GETARG_Bx(inst1);

            if ((op2 == OP_LT || op2 == OP_LE) && bx < PROTO_CONST_COUNT(proto)) {
                int a = GETARG_A(inst2);
                int b = GETARG_B(inst2);
                int k = GETARG_C(inst2);

                // Check: LOADK's result is the second operand
                if (b == reg_k) {
                    XrValue kval = PROTO_CONSTANT(proto, bx);
                    int imm;

                    if (xr_fusion_is_small_int(kval, &imm)) {
                        OpCode new_op = OP_NOP;
                        switch (op2) {
                            case OP_LT:
                                new_op = OP_LTI;
                                break;
                            case OP_LE:
                                new_op = OP_LEI;
                                break;
                            default:
                                break;
                        }

                        if (new_op != OP_NOP) {
                            PROTO_CODE(proto, pc) = CREATE_ABC(OP_NOP, 0, 0, 0);
                            PROTO_SET_CODE(proto, pc + 1, CREATE_ABC(new_op, a, imm, k));
                            opt_count++;
                            g_fusion_stats.cmp_to_imm++;
                        }
                    }
                }
            }
        }
    }

    return opt_count;
}

// Main optimization function
int xr_fusion_optimize(XrProto *proto) {
    if (!proto || PROTO_CODE_COUNT(proto) == 0) {
        return 0;
    }

    int total = 0;

    // Execute various fusion optimizations
    total += xr_fusion_loadk_const(proto);
    total += xr_fusion_arith_imm(proto);
    total += xr_fusion_cmp_const(proto);

    g_fusion_stats.total_fusions = total;

    // Recursively optimize nested functions
    for (int i = 0; i < PROTO_PROTO_COUNT(proto); i++) {
        total += xr_fusion_optimize(PROTO_PROTO(proto, i));
    }

    return total;
}

/* ========== Statistics ========== */

void xr_fusion_reset_stats(void) {
    memset(&g_fusion_stats, 0, sizeof(FusionStats));
}

void xr_fusion_print_stats(void) {
    if (g_fusion_stats.total_fusions > 0) {
        printf("\n=== Instruction Fusion Statistics ===\n");
        printf("LOADK to LOADI: %d\n", g_fusion_stats.loadk_to_loadi);
        printf("Arith to immediate: %d\n", g_fusion_stats.arith_to_imm);
        printf("Compare to immediate: %d\n", g_fusion_stats.cmp_to_imm);
        printf("Total fusions: %d\n", g_fusion_stats.total_fusions);
        printf("=====================================\n");
    }
}
