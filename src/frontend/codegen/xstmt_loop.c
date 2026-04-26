/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xstmt_loop.c - While and for loop compilation
 *
 * KEY CONCEPT:
 *   Compiles while loops and C-style for loops, including FORPREP/FORLOOP
 *   optimization for numeric loops. Extracted from xstmt_control.c.
 */

#include "xstmt.h"
#include "../../base/xchecks.h"
#include "../../runtime/xisolate_api.h"
#include "xcompiler.h"
#include "xcompiler_context.h"
#include "xemit.h"
#include "xexpr.h"
#include "xexpr_desc.h"
#include "../../runtime/symbol/xsymbol_table.h"
#include "../../runtime/value/xtype.h"
#include "../analyzer/xanalyzer.h"
#include <stdio.h>

/* ========== Condition Jump Chain (for while/if) ========== */

/*
 * Emit a single leaf condition as comparison + JMP.
 * Returns PC of the emitted JMP instruction.
 *
 * For comparison nodes (LT/LE/GT/GE/EQ/NE): emit OP_LT/LE + JMP (2 instructions)
 * For other expressions: emit compile_expr + TEST + JMP (3 instructions)
 *
 * k_extra: XOR with comparison's natural k-flag.
 *   0 = normal: JMP executes when condition is FALSE
 *   1 = inverted: JMP executes when condition is TRUE (for || short-circuit)
 */
static int emit_leaf_cond_jump(XrCompilerContext *ctx, XrCompiler *compiler, AstNode *cond,
                               int k_extra) {
    XR_DCHECK(ctx != NULL, "emit_leaf_cond_jump: NULL ctx");
    XR_DCHECK(compiler != NULL, "emit_leaf_cond_jump: NULL compiler");
    XR_DCHECK(cond != NULL, "emit_leaf_cond_jump: NULL cond");
    if (cond->type >= AST_BINARY_LT && cond->type <= AST_BINARY_GE) {
        BinaryNode *bin = &cond->as.binary;

        // Const propagation: resolve const variables to literals in-place
        // so LTI/LEI immediate comparison can trigger
        for (int side = 0; side < 2; side++) {
            AstNode *op = (side == 0) ? bin->left : bin->right;
            if (op->type == AST_VARIABLE) {
                XrLocalInfo *li = compiler_get_local_by_name(compiler, op->as.variable.name);
                if (li && li->is_const) {
                    if (li->comptime.type == COMPTIME_INT) {
                        op->type = AST_LITERAL_INT;
                        op->as.literal.kind = LITERAL_KIND_INT;
                        op->as.literal.raw_value.int_val = li->comptime.as.int_val;
                    } else if (li->comptime.type == COMPTIME_FLOAT) {
                        op->type = AST_LITERAL_FLOAT;
                        op->as.literal.kind = LITERAL_KIND_FLOAT;
                        op->as.literal.raw_value.float_val = li->comptime.as.float_val;
                    }
                } else {
                    XrString *ns = xr_compile_time_intern(ctx->X, op->as.variable.name,
                                                          strlen(op->as.variable.name));
                    ConstEntry *ce = xr_compiler_ctx_find_const(ctx, ns);
                    if (ce) {
                        if (ce->type == CONST_INT) {
                            op->type = AST_LITERAL_INT;
                            op->as.literal.kind = LITERAL_KIND_INT;
                            op->as.literal.raw_value.int_val = ce->value.int_val;
                        } else if (ce->type == CONST_FLOAT) {
                            op->type = AST_LITERAL_FLOAT;
                            op->as.literal.kind = LITERAL_KIND_FLOAT;
                            op->as.literal.raw_value.float_val = ce->value.float_val;
                        }
                    }
                }
            }
        }

        XrExprDesc left_e = xr_compile_expr(ctx, compiler, bin->left);
        int ra = xexpr_to_anyreg_readonly(ctx, compiler, &left_e);

        // Check for small integer constant on right (also float with integer value)
        AstNode *right = bin->right;
        bool use_imm = false;
        int imm_value = 0;
        if (right->type == AST_LITERAL_INT) {
            int64_t val = right->as.literal.raw_value.int_val;
            if (val >= -128 && val <= 127) {
                use_imm = true;
                imm_value = (int) val;
            }
        } else if (right->type == AST_LITERAL_FLOAT) {
            double fval = right->as.literal.raw_value.float_val;
            int ival = (int) fval;
            if (fval == (double) ival && ival >= -128 && ival <= 127) {
                use_imm = true;
                imm_value = ival;
            }
        }

        OpCode cmp_op, cmp_op_imm;
        bool swap_reg = false;
        int k = k_extra;
        int k_imm = k_extra;

        bool left_i64 = xexpr_is_raw_i64(&left_e);
        bool left_f64 = xexpr_is_raw_f64(&left_e);

        switch (cond->type) {
            case AST_BINARY_LT:
                cmp_op = OP_LT;
                cmp_op_imm = OP_LTI;
                break;
            case AST_BINARY_LE:
                cmp_op = OP_LE;
                cmp_op_imm = OP_LEI;
                break;
            case AST_BINARY_GT:
                cmp_op = OP_LT;
                cmp_op_imm = OP_LEI;
                swap_reg = true;
                k_imm ^= 1;
                break;
            case AST_BINARY_GE:
                cmp_op = OP_LE;
                cmp_op_imm = OP_LTI;
                swap_reg = true;
                k_imm ^= 1;
                break;
            default:
                cmp_op = OP_LT;
                cmp_op_imm = OP_LTI;
                break;
        }

        if (use_imm) {
            if (left_i64 || left_f64) {
                ra = xexpr_ensure_boxed(ctx, compiler, &left_e, ra);
            }
            emit_abc(compiler->emitter, cmp_op_imm, ra, imm_value & 0xFF, k_imm);
        } else {
            XrExprDesc right_e = xr_compile_expr(ctx, compiler, right);
            int rb = xexpr_to_anyreg_readonly(ctx, compiler, &right_e);

            ra = xexpr_ensure_boxed(ctx, compiler, &left_e, ra);
            rb = xexpr_ensure_boxed(ctx, compiler, &right_e, rb);

            if (swap_reg)
                emit_abc(compiler->emitter, cmp_op, rb, ra, k);
            else
                emit_abc(compiler->emitter, cmp_op, ra, rb, k);
        }
        return emit_jump(compiler->emitter, OP_JMP);
    }

    if ((cond->type == AST_BINARY_EQ || cond->type == AST_BINARY_NE) &&
        cond->as.binary.right->type == AST_LITERAL_NULL) {
        XrExprDesc left_e = xr_compile_expr(ctx, compiler, cond->as.binary.left);
        int ra = xexpr_to_anyreg_readonly(ctx, compiler, &left_e);
        int k = (cond->type == AST_BINARY_NE) ? 1 : 0;
        k ^= k_extra;
        xemit_isnull(compiler->emitter, ra, k);
        return emit_jump(compiler->emitter, OP_JMP);
    }

    if (cond->type == AST_BINARY_EQ || cond->type == AST_BINARY_NE) {
        BinaryNode *bin = &cond->as.binary;
        XrExprDesc left_e = xr_compile_expr(ctx, compiler, bin->left);
        int ra = xexpr_to_anyreg_readonly(ctx, compiler, &left_e);
        XrExprDesc right_e = xr_compile_expr(ctx, compiler, bin->right);
        int rb = xexpr_to_anyreg_readonly(ctx, compiler, &right_e);
        ra = xexpr_ensure_boxed(ctx, compiler, &left_e, ra);
        rb = xexpr_ensure_boxed(ctx, compiler, &right_e, rb);

        int k = k_extra;
        if (cond->type == AST_BINARY_NE)
            k ^= 1;
        xemit_eq(compiler->emitter, ra, rb, k);
        return emit_jump(compiler->emitter, OP_JMP);
    }

    // Generic: compile expression to register, then TEST + JMP
    XrExprDesc e = xr_compile_expr(ctx, compiler, cond);
    int reg = xexpr_to_anyreg(ctx, compiler, &e);
    xemit_test(compiler->emitter, reg, k_extra);
    return emit_jump(compiler->emitter, OP_JMP);
}

/*
 * Compile boolean condition as a jump chain (recursive).
 *
 * Instead of computing a bool value into a register, this generates
 * a series of conditional jumps that directly control flow.
 *
 * false_list: accumulated JMPs for "condition is false" (→ exit)
 * true_list:  accumulated JMPs for "condition is true"  (→ body, for || short-circuit)
 *
 * After call, execution falls through when condition is TRUE.
 *
 * For &&: each false sub-condition jumps to exit
 * For ||: each true sub-condition jumps to body (skipping remaining conditions)
 */
static void compile_cond_chain(XrCompilerContext *ctx, XrCompiler *compiler, AstNode *cond,
                               int *false_list, int *true_list) {
    if (cond->type == AST_BINARY_AND) {
        BinaryNode *bin = &cond->as.binary;
        // A && B: A false → exit, A true → fall through to B
        compile_cond_chain(ctx, compiler, bin->left, false_list, true_list);
        // If || inside left added true_list entries, they should
        // jump to HERE (start of right), not to body
        if (*true_list != NO_JUMP) {
            patch_jump_list(compiler->emitter, *true_list, -1);
            *true_list = NO_JUMP;
        }
        compile_cond_chain(ctx, compiler, bin->right, false_list, true_list);
    } else if (cond->type == AST_BINARY_OR) {
        BinaryNode *bin = &cond->as.binary;
        // A || B: A true → body (skip B), A false → fall through to B
        int left_false = NO_JUMP;
        compile_cond_chain(ctx, compiler, bin->left, &left_false, true_list);
        // Left true fell through → skip right, jump to body
        int skip_right = emit_jump(compiler->emitter, OP_JMP);
        *true_list = jump_list_concat(compiler->emitter, *true_list, skip_right);
        // Left false → patch to here (start of right)
        patch_jump_list(compiler->emitter, left_false, -1);
        // Right: false → exit, true → fall through to body
        compile_cond_chain(ctx, compiler, bin->right, false_list, true_list);
    } else {
        // Leaf condition: emit comparison/test + JMP → false_list
        int jmp = emit_leaf_cond_jump(ctx, compiler, cond, 0);
        *false_list = jump_list_concat(compiler->emitter, *false_list, jmp);
    }
}

/*
 * Check if a condition contains && or || that benefits from jump chain optimization.
 */
static bool is_logical_condition(AstNode *cond) {
    return cond->type == AST_BINARY_AND || cond->type == AST_BINARY_OR;
}

/* ========== While Loop ========== */

/*
 * Compile while loop
 *
 * Generated code:
 * loop_start:
 *   cond_reg = <condition>
 *   TEST cond_reg 0
 *   JMP exit
 *   <body>
 *   JMP loop_start
 * exit:
 */
void compile_while(XrCompilerContext *ctx, XrCompiler *compiler, WhileStmtNode *node) {
    XR_DCHECK(ctx != NULL, "compile_while: NULL ctx");
    XR_DCHECK(compiler != NULL, "compile_while: NULL compiler");
    int loop_start;
    int exit_jump;

    /* Optimization: Use OP_LT/OP_LE/OP_GT/OP_GE + JMP (2 instructions)
     * instead of OP_CMP_LT + OP_TEST + JMP (3 instructions)
     *
     * Key optimizations:
     * 1. Use LTI/LEI/GTI/GEI for small integer constants (-128~127)
     * 2. Load constants outside loop, loop_start points to compare instruction
     * 3. != null optimization: Use OP_EQK + JMP (2 instructions)
     */
    AstNode *cond = node->condition;

    // Linked list loop optimization: while (current != null) or while (current == null)
    if ((cond->type == AST_BINARY_NE || cond->type == AST_BINARY_EQ) &&
        cond->as.binary.right->type == AST_LITERAL_NULL) {
        loop_start = PROTO_CODE_COUNT(compiler->proto);

        // Compile left operand
        XrExprDesc left_e = xr_compile_expr(ctx, compiler, cond->as.binary.left);
        int ra = xexpr_to_anyreg_readonly(ctx, compiler, &left_e);

        /* OP_ISNULL A k: if (R[A] == null) != k then PC++ (skip next instruction)
         *
         * while (current != null): exit when current == null
         *   - k=1: if (is_null) != 1 then skip, else execute JMP to exit
         *
         * while (current == null): exit when current != null
         *   - k=0: if (is_null) != 0 then skip, else execute JMP to exit
         */
        int k = (cond->type == AST_BINARY_NE) ? 1 : 0;
        xemit_isnull(compiler->emitter, ra, k);

        exit_jump = emit_jump(compiler->emitter, OP_JMP);

        /* Critical fix: Update local_end to current freereg.
         * This ensures local variables defined in loop body are allocated
         * from higher registers, avoiding conflicts with registers used by
         * condition check code (and parameter preparation code after loop_start).
         *
         * Problem scenario: Code after loop_start uses temp registers,
         * but these registers get reused by local variables in loop body.
         * When loop jumps back, code after loop_start executes again,
         * overwriting local variable values.
         */
        if (compiler->regalloc) {
            int current_freereg = xreg_get_freereg(compiler->regalloc);
            xreg_set_local_end(compiler->regalloc, current_freereg);
        }

        goto compile_body;  // Jump to loop body compilation
    }

    if (cond->type >= AST_BINARY_LT && cond->type <= AST_BINARY_GE) {
        BinaryNode *bin = &cond->as.binary;

        /* Important: loop_start must be set before compiling left operand
         * because loop needs to reload variable values when jumping back
         */
        loop_start = PROTO_CODE_COUNT(compiler->proto);

        // Compile left operand
        XrExprDesc left_e = xr_compile_expr(ctx, compiler, bin->left);
        int ra = xexpr_to_anyreg_readonly(ctx, compiler, &left_e);

        // Check if right operand is small integer constant
        AstNode *right = bin->right;
        bool use_imm = false;
        int imm_value = 0;

        if (right->type == AST_LITERAL_INT) {
            int64_t val = right->as.literal.raw_value.int_val;
            if (val >= -128 && val <= 127) {
                use_imm = true;
                imm_value = (int) val;
            }
        }

        // Select comparison instruction
        OpCode cmp_op, cmp_op_imm;
        bool swap_reg = false;
        int k_imm = 0;

        // Check if left operand is typed for native comparison
        bool left_typed_i64 = xexpr_is_raw_i64(&left_e);
        bool left_typed_f64 = xexpr_is_raw_f64(&left_e);

        switch (cond->type) {
            case AST_BINARY_LT:
                cmp_op = OP_LT;
                cmp_op_imm = OP_LTI;
                break;
            case AST_BINARY_LE:
                cmp_op = OP_LE;
                cmp_op_imm = OP_LEI;
                break;
            case AST_BINARY_GT:
                cmp_op = OP_LT;
                cmp_op_imm = OP_LEI;
                swap_reg = true;
                k_imm = 1;
                break;
            case AST_BINARY_GE:
                cmp_op = OP_LE;
                cmp_op_imm = OP_LTI;
                swap_reg = true;
                k_imm = 1;
                break;
            default:
                cmp_op = OP_LT;
                cmp_op_imm = OP_LTI;
                break;
        }

        if (use_imm) {
            if (left_typed_i64 || left_typed_f64) {
                ra = xexpr_ensure_boxed(ctx, compiler, &left_e, ra);
            }
            emit_abc(compiler->emitter, cmp_op_imm, ra, imm_value & 0xFF, k_imm);
        } else {
            XrExprDesc right_e = xr_compile_expr(ctx, compiler, right);
            int rb = xexpr_to_anyreg_readonly(ctx, compiler, &right_e);

            ra = xexpr_ensure_boxed(ctx, compiler, &left_e, ra);
            rb = xexpr_ensure_boxed(ctx, compiler, &right_e, rb);

            if (swap_reg)
                emit_abc(compiler->emitter, cmp_op, rb, ra, 0);
            else
                emit_abc(compiler->emitter, cmp_op, ra, rb, 0);
        }

        exit_jump = emit_jump(compiler->emitter, OP_JMP);

        // Critical fix: Update local_end to current freereg (same as above)
        if (compiler->regalloc) {
            int current_freereg = xreg_get_freereg(compiler->regalloc);
            xreg_set_local_end(compiler->regalloc, current_freereg);
        }
    } else if (is_logical_condition(cond)) {
        // Optimized path: compile && / || as jump chain
        // Instead of computing a bool value, emit direct conditional jumps
        loop_start = PROTO_CODE_COUNT(compiler->proto);

        int false_list = NO_JUMP;
        int true_list = NO_JUMP;
        compile_cond_chain(ctx, compiler, cond, &false_list, &true_list);

        // Patch true_list (|| short-circuit) to body start (current PC)
        if (true_list != NO_JUMP) {
            patch_jump_list(compiler->emitter, true_list, -1);
        }

        // Critical fix: Update local_end to current freereg
        if (compiler->regalloc) {
            int current_freereg = xreg_get_freereg(compiler->regalloc);
            xreg_set_local_end(compiler->regalloc, current_freereg);
        }

        // Compile body
        XrLoopState loop_state;
        loop_state_save(compiler, &loop_state);
        loop_state_enter(compiler, loop_start);
        xr_compile_statement(ctx, compiler, node->body);
        loop_state_restore(compiler, &loop_state);

        loop_state_patch_continue(compiler, &loop_state, loop_start);
        emit_loop(compiler->emitter, loop_start);
        // Patch all false jumps to exit (current PC after loop jump)
        patch_jump_list(compiler->emitter, false_list, -1);
        loop_state_patch_break(compiler, &loop_state, -1);
        return;
    } else {
        // Generic path: Compile condition expression

        /* Optimization: For while(true) constant condition loop, set loop_start
         * after condition check. This avoids re-executing unnecessary code when
         * looping back, preventing register overwrite issues.
         */
        bool is_constant_true = (cond->type == AST_LITERAL_TRUE);

        if (is_constant_true) {
            // while(true) optimization: condition check executes only once
            XrExprDesc cond_expr = xr_compile_expr(ctx, compiler, cond);
            int cond_reg = xexpr_to_anyreg(ctx, compiler, &cond_expr);
            xemit_test(compiler->emitter, cond_reg, 0);
            exit_jump = emit_jump(compiler->emitter, OP_JMP);

            // loop_start set after condition check, before loop body
            loop_start = PROTO_CODE_COUNT(compiler->proto);
        } else {
            // Non-constant condition: loop_start before condition check
            loop_start = PROTO_CODE_COUNT(compiler->proto);

            XrExprDesc cond_expr = xr_compile_expr(ctx, compiler, cond);
            int cond_reg = xexpr_to_anyreg(ctx, compiler, &cond_expr);
            xemit_test(compiler->emitter, cond_reg, 0);
            exit_jump = emit_jump(compiler->emitter, OP_JMP);
        }
    }

compile_body:;  // Empty statement (C label requires statement)

    XrLoopState loop_state;
    loop_state_save(compiler, &loop_state);
    loop_state_enter(compiler, loop_start);
    xr_compile_statement(ctx, compiler, node->body);
    loop_state_restore(compiler, &loop_state);

    loop_state_patch_continue(compiler, &loop_state, loop_start);
    emit_loop(compiler->emitter, loop_start);
    patch_jump(compiler->emitter, exit_jump, -1);
    loop_state_patch_break(compiler, &loop_state, -1);
}

/* ========== For Loop (standard CMP+JMP) ========== */

/*
 * Compile for loop (generic version).
 *
 * for (init; cond; incr) body
 *
 * Generated code:
 *   <init>
 * loop_start:
 *   cond_reg = <condition>
 *   TEST cond_reg 0
 *   JMP exit
 *   <body>
 *   <increment>
 *   JMP loop_start
 * exit:
 */
void compile_for(XrCompilerContext *ctx, XrCompiler *compiler, ForStmtNode *node) {
    XR_DCHECK(ctx != NULL, "compile_for: NULL ctx");
    XR_DCHECK(compiler != NULL, "compile_for: NULL compiler");
    // All for loops use the generic CMP+JMP path.
    // FORPREP/FORLOOP fused instructions removed: they hide multiple
    // side-effects in one opcode, making JIT/AOT decomposition fragile
    // and causing type confusion bugs with flow-insensitive slot types.

    // Generic for loop compilation
    // Enter loop scope
    scope_begin(compiler);

    // Compile initializer
    if (node->initializer != NULL) {
        xr_compile_statement(ctx, compiler, node->initializer);
    }

    int loop_start = PROTO_CODE_COUNT(compiler->proto);

    // Compile condition
    int exit_jump = -1;
    if (node->condition != NULL) {
        XrExprDesc cond_expr = xr_compile_expr(ctx, compiler, node->condition);
        exit_jump = xexpr_goiftrue(ctx, compiler, &cond_expr);
    }

    XrLoopState loop_state;
    loop_state_save(compiler, &loop_state);
    loop_state_enter(compiler, loop_start);
    xr_compile_statement(ctx, compiler, node->body);
    loop_state_restore(compiler, &loop_state);

    // Increment expression start position
    int increment_pos = PROTO_CODE_COUNT(compiler->proto);
    loop_state_patch_continue(compiler, &loop_state, increment_pos);

    // Compile increment if present (assignment expressions are not compatible with XrExprDesc)
    if (node->increment != NULL) {
        int inc_reg = xr_compile_expression(ctx, compiler, node->increment);
        xreg_set_freereg(compiler->regalloc, inc_reg);
    }

    // Jump back to loop start
    emit_loop(compiler->emitter, loop_start);

    // Patch exit jump
    if (exit_jump >= 0) {
        patch_jump(compiler->emitter, exit_jump, -1);
    }

    loop_state_patch_break(compiler, &loop_state, -1);

    // Exit loop scope
    scope_end(ctx, compiler);
}
