/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xexpr_binary.c - Binary expression compilation
 *
 * KEY CONCEPT:
 *   Handles all binary operations: arithmetic (+, -, *, /, %), comparison
 *   (==, !=, <, >, <=, >=), and logical (&&, || with short-circuit evaluation).
 *   Optimizations include constant folding, immediate operands, short-circuit
 *   evaluation, and STRBUF batch string concatenation.
 */

#include "xexpr.h"
#include "../../base/xchecks.h"
#include "../../base/xlog.h"
#include "../../runtime/xisolate_api.h"
#include "xcompiler.h"
#include "xcompiler_context.h"
#include "xemit.h"
#include "xoptimize.h"
#include "xexpr_desc.h"
#include "../xdiag_fmt.h"
#include "../../runtime/symbol/xsymbol_table.h"
#include "../../runtime/value/xtype.h"
#include "../../runtime/value/xslot_type.h"
#include <stdio.h>

// Thin wrapper for the public xexpr_ensure_boxed
static inline int ensure_boxed(XrCompilerContext *ctx, XrCompiler *compiler,
                               XrExprDesc *e, int reg) {
    return xexpr_ensure_boxed(ctx, compiler, e, reg);
}

/* ========== STRBUF Optimization: Batch String Concatenation ========== */

/*
 * Check if node is part of a string concatenation chain.
 * Only AST_BINARY_ADD can be string concatenation.
 */
static bool is_concat_node(AstNode *node) {
    return node && node->type == AST_BINARY_ADD;
}

/*
 * Collect all operands in a string concatenation chain.
 *
 * Example: a + b + c + d
 * AST structure:
 *        +
 *       / \
 *      +   d
 *     / \
 *    +   c
 *   / \
 *  a   b
 *
 * Collection order: [a, b, c, d] (left to right, depth first)
 *
 * @param node Current node
 * @param operands Operand array (output parameter)
 * @param count Current operand count (input/output parameter)
 * @param max_count Maximum operand count (255, B field limit)
 * @return Whether collection succeeded
 */
static bool collect_concat_operands(AstNode *node, AstNode **operands, int *count, int max_count) {
    if (!node) {
        return false;
    }

    // If it's an ADD node, recursively collect
    if (is_concat_node(node)) {
        BinaryNode *binary = &node->as.binary;

        // First collect left subtree
        if (!collect_concat_operands(binary->left, operands, count, max_count)) {
            return false;
        }

        // Then collect right subtree
        if (!collect_concat_operands(binary->right, operands, count, max_count)) {
            return false;
        }

        return true;
    }

    // Not an ADD node, treat as leaf operand
    if (*count >= max_count) {
        return false;  // Exceeded maximum count
    }

    operands[(*count)++] = node;
    return true;
}

/*
 * Compile string concatenation chain (optimized version with type awareness).
 *
 * Strategy:
 * 1. Collect all operands into array
 * 2. Type-aware optimization:
 *    - All number literals -> don't use CONCAT (fall back to ADD)
 *    - Has string literal -> use CONCAT
 *    - All variables/expressions -> conservative strategy, don't use CONCAT
 * 3. Compile all operands to consecutive registers
 * 4. Generate STRBUF_NEW + STRBUF_APPEND*N + STRBUF_FINISH sequence
 *
 * Advantages:
 * - merge all operands at once
 * - Reduce intermediate string objects
 * - Reduce instruction count
 * - Fix bug where numeric addition incorrectly used CONCAT
 *
 * @return Result register (first operand's register), -1 means don't use CONCAT
 */
static int compile_concat_chain(XrCompilerContext *ctx, XrCompiler *compiler, AstNode *node) {
    XR_DCHECK(ctx != NULL, "compile_concat_chain: NULL ctx");
    XR_DCHECK(compiler != NULL, "compile_concat_chain: NULL compiler");
    // Collect all operands
    AstNode *operands[255];  // B field 8 bits, max 255 operands
    int count = 0;

    if (!collect_concat_operands(node, operands, &count, 255)) {
        // Collection failed (exceeded 255), fall back to normal compilation
        return -1;
    }

    if (count < 2) {
        // Less than 2 operands, don't need CONCAT
        return -1;
    }

    // === Type-aware optimization: check if CONCAT should be used ===
    bool has_string_literal = false;  // Has string literal
    bool has_only_numbers = true;     // All number literals

    for (int i = 0; i < count; i++) {
        AstNodeType t = operands[i]->type;

        // Check for string literal
        if (t == AST_LITERAL_STRING) {
            has_string_literal = true;
            has_only_numbers = false;
        }
        // Check for number literal
        else if (t == AST_LITERAL_INT || t == AST_LITERAL_FLOAT) {
            // Continue checking other operands
        }
        // Variables, expressions, function calls, etc.: type unknown
        else {
            has_only_numbers = false;
        }
    }

    // === Decision rules ===

    // Rule 1: All number literals -> don't use CONCAT, fall back to normal ADD
    if (has_only_numbers) {
        return -1;  // Let regular ADD/ADDI instructions handle
    }

    // Rule 2: No string literal -> check type info via get_expr_type
    // (uses Codegen local info which is more reliable than raw AST compile_type)
    if (!has_string_literal) {
        bool has_string_type = false;
        for (int i = 0; i < count; i++) {
            XrType *ct = get_expr_type(ctx, compiler, operands[i]);
            if (ct && XR_TYPE_IS_STRING(ct)) {
                has_string_type = true;
                break;
            }
        }
        if (!has_string_type) {
            return -1;  // Truly unknown types, let ADD handle at runtime
        }
    }

    // Rule 3: check if all operands are confirmed string for CONCAT optimization
    bool all_confirmed_string = true;
    for (int i = 0; i < count; i++) {
        if (operands[i]->type == AST_LITERAL_STRING) continue;
        XrType *ct = get_expr_type(ctx, compiler, operands[i]);
        if (!ct || !XR_TYPE_IS_STRING(ct)) {
            all_confirmed_string = false;
        }
    }

    // If any operand is unknown type, fall back to OP_ADD for runtime type checking
    if (!all_confirmed_string) {
        return -1;
    }

    // All operands confirmed string, use STRBUF sequence:
    // STRBUF_NEW R[buf]; STRBUF_APPEND R[buf] R[op_i]; ...; STRBUF_FINISH R[buf]

    int buf_reg = reg_alloc(ctx, compiler);
    emit_abc(compiler->emitter, OP_STRBUF_NEW, buf_reg, 0, 0);

    for (int i = 0; i < count; i++) {
        XrExprDesc expr = xr_compile_expr(ctx, compiler, operands[i]);
        int op_reg = xexpr_to_anyreg(ctx, compiler, &expr);
        emit_abc(compiler->emitter, OP_STRBUF_APPEND, buf_reg, op_reg, 0);
        if (op_reg != buf_reg) {
            reg_free(compiler, op_reg);
        }
    }

    emit_abc(compiler->emitter, OP_STRBUF_FINISH, buf_reg, 0, 0);

    return buf_reg;
}

/* ========== Logical Operations (Short-Circuit Evaluation) ========== */

/*
 * Compile logical AND (&&) - using jump list optimization.
 *
 * Short-circuit evaluation: if left operand is false, don't evaluate right operand.
 *
 * Optimization strategy:
 * 1. Use jump list to manage multiple jumps to false branch
 * 2. Support chained && operation (a && b && c) optimization
 *
 * Generated code:
 *   rb = <left>
 *   TESTSET rb, rb, 1     ; if false then skip
 *   JMP [false_list]      ; add to false list
 *   rc = <right>
 *   MOVE rb, rc
 *   NOT rb, rb            ; convert to bool
 *   NOT rb, rb
 * false_list:
 *   ; result in rb (bool)
 */
int compile_and(XrCompilerContext *ctx, XrCompiler *compiler, BinaryNode *node) {
    int false_list = NO_JUMP;  // False jump list

    // Compile left operand
    XrExprDesc left_expr = xr_compile_expr(ctx, compiler, node->left);
    int rb = xexpr_to_anyreg(ctx, compiler, &left_expr);

    // Test left operand: if false, jump
    emit_abc(compiler->emitter, OP_TESTSET, rb, rb, 0);
    int jump = emit_jump(compiler->emitter, OP_JMP);
    false_list = jump;  // Initialize false list

    // Compile right operand
    XrExprDesc right_expr = xr_compile_expr(ctx, compiler, node->right);
    int rc = xexpr_to_anyreg(ctx, compiler, &right_expr);

    // Move result to rb, then convert to bool (NOT+NOT)
    emit_move(compiler->emitter, rb, rc);
    emit_abc(compiler->emitter, OP_NOT, rb, rb, 0);
    emit_abc(compiler->emitter, OP_NOT, rb, rb, 0);

    // Set freereg = rb + 1, reclaim rc
    xreg_set_freereg(compiler->regalloc, rb + 1);

    // Patch false jump list (jump to current position)
    patch_jump_list(compiler->emitter, false_list, -1);

    return rb;
}

/*
 * Compile logical OR (||) - using jump list optimization.
 *
 * Short-circuit evaluation: if left operand is true, don't evaluate right operand.
 *
 * Optimization strategy:
 * 1. Use jump list to manage multiple jumps to true branch
 * 2. Support chained || operation (a || b || c) optimization
 *
 * Generated code:
 *   rb = <left>
 *   TESTSET rb, rb, 0     ; if true then skip
 *   JMP [true_list]       ; add to true list
 *   rc = <right>
 *   MOVE rb, rc
 *   NOT rb, rb            ; convert to bool
 *   NOT rb, rb
 * true_list:
 *   ; result in rb (bool)
 */
int compile_or(XrCompilerContext *ctx, XrCompiler *compiler, BinaryNode *node) {
    int true_list = NO_JUMP;  // True jump list

    // Compile left operand
    XrExprDesc left_expr = xr_compile_expr(ctx, compiler, node->left);
    int rb = xexpr_to_anyreg(ctx, compiler, &left_expr);

    // Test left operand: if true, jump
    emit_abc(compiler->emitter, OP_TESTSET, rb, rb, 1);
    int jump = emit_jump(compiler->emitter, OP_JMP);
    true_list = jump;  // Initialize true list

    // Compile right operand
    XrExprDesc right_expr = xr_compile_expr(ctx, compiler, node->right);
    int rc = xexpr_to_anyreg(ctx, compiler, &right_expr);

    // Move result to rb, then convert to bool (NOT+NOT)
    emit_move(compiler->emitter, rb, rc);
    emit_abc(compiler->emitter, OP_NOT, rb, rb, 0);
    emit_abc(compiler->emitter, OP_NOT, rb, rb, 0);

    // Set freereg = rb + 1, reclaim rc
    xreg_set_freereg(compiler->regalloc, rb + 1);

    // Patch true jump list (jump to current position)
    patch_jump_list(compiler->emitter, true_list, -1);

    return rb;
}

/* ========== Arithmetic Operations ========== */

/*
 * Compile binary expression (returns XrExprDesc).
 *
 * True RELOC implementation:
 * - Emit instruction with A=0 (target register pending)
 * - Return XEXPR_RELOC, record instruction PC
 * - Allocate target register and write back on discharge
 *
 * This avoids MOVE instruction in `var x = a + b` scenario.
 */
XrExprDesc compile_binary(XrCompilerContext *ctx, XrCompiler *compiler, BinaryNode *node, AstNodeType type) {
    XR_DCHECK(ctx != NULL, "compile_binary: NULL ctx");
    XR_DCHECK(compiler != NULL, "compile_binary: NULL compiler");
    XR_DCHECK(node != NULL, "compile_binary: NULL node");
    XrExprDesc e = {0};
    xexpr_init(&e, XEXPR_VOID, -1);

    // Early type inference for inst_types propagation.
    // Arithmetic ops on known-type operands produce known-type results.
    XrType *result_ct = NULL;
    if (type >= AST_BINARY_ADD && type <= AST_BINARY_RSHIFT) {
        XrType *lct = get_expr_type(ctx, compiler, node->left);
        XrType *rct = get_expr_type(ctx, compiler, node->right);
        if (lct && rct) {
            bool li = XR_TYPE_IS_INT(lct), lf = XR_TYPE_IS_FLOAT(lct);
            bool ri = XR_TYPE_IS_INT(rct), rf = XR_TYPE_IS_FLOAT(rct);
            if (li && ri) result_ct = (XrType *)xr_type_new_int();
            else if ((li || lf) && (ri || rf)) result_ct = (XrType *)xr_type_new_float();
        }
    }

    // ===== Logical operations need short-circuit evaluation, keep old implementation =====
    if (type == AST_BINARY_AND) {
        int reg = compile_and(ctx, compiler, node);
        xexpr_init(&e, XEXPR_TEMP, reg);
        e.compile_type = result_ct;
        return e;
    }
    if (type == AST_BINARY_OR) {
        int reg = compile_or(ctx, compiler, node);
        xexpr_init(&e, XEXPR_TEMP, reg);
        e.compile_type = result_ct;
        return e;
    }

    // ===== Optimization 0: STRBUF batch string concatenation =====
    if (type == AST_BINARY_ADD) {
        AstNode temp_node;
        temp_node.type = type;
        temp_node.as.binary = *node;

        int concat_result = compile_concat_chain(ctx, compiler, &temp_node);
        if (concat_result >= 0) {
            // CONCAT already allocated target register, return TEMP
            xexpr_init(&e, XEXPR_TEMP, concat_result);
            e.compile_type = result_ct;
            return e;
        }
    }

    // ===== Optimization 0b: string repeat OP_STR_REPEAT =====
    // Detect pattern: string * int or int * string
    if (type == AST_BINARY_MUL) {
        bool is_str_mul_int = (node->left->type == AST_LITERAL_STRING &&
                               node->right->type == AST_LITERAL_INT);
        bool is_int_mul_str = (node->left->type == AST_LITERAL_INT &&
                               node->right->type == AST_LITERAL_STRING);

        if (is_str_mul_int || is_int_mul_str) {
            // Compile string and integer to registers
            AstNode *str_node = is_str_mul_int ? node->left : node->right;
            AstNode *int_node = is_str_mul_int ? node->right : node->left;

            XrExprDesc str_e = xr_compile_expr(ctx, compiler, str_node);
            int rb = xexpr_to_anyreg_readonly(ctx, compiler, &str_e);

            XrExprDesc int_e = xr_compile_expr(ctx, compiler, int_node);
            int rc = xexpr_to_anyreg_readonly(ctx, compiler, &int_e);

            // Emit OP_STR_REPEAT, A=0 pending relocation
            int pc = emit_abc(compiler->emitter, OP_STR_REPEAT, 0, rb, rc);

            e.kind = XEXPR_RELOC;
            e.u.pc = pc;
            return e;
        }

        // Runtime detection: left operand type is STRING
        XrType *left_ct = get_expr_type(ctx, compiler, node->left);
        XrType *right_ct = get_expr_type(ctx, compiler, node->right);
        bool left_is_string = left_ct && (left_ct->kind == XR_KIND_STRING);
        bool left_is_int = left_ct && (left_ct->kind == XR_KIND_INT);
        bool right_is_string = right_ct && (right_ct->kind == XR_KIND_STRING);
        bool right_is_int = right_ct && (right_ct->kind == XR_KIND_INT);

        if (left_is_string && right_is_int) {
            XrExprDesc str_e = xr_compile_expr(ctx, compiler, node->left);
            int rb = xexpr_to_anyreg_readonly(ctx, compiler, &str_e);

            XrExprDesc int_e = xr_compile_expr(ctx, compiler, node->right);
            int rc = xexpr_to_anyreg_readonly(ctx, compiler, &int_e);

            int pc = emit_abc(compiler->emitter, OP_STR_REPEAT, 0, rb, rc);
            e.kind = XEXPR_RELOC;
            e.u.pc = pc;
            return e;
        }
        if (left_is_int && right_is_string) {
            // int * string form, swap order
            XrExprDesc str_e = xr_compile_expr(ctx, compiler, node->right);
            int rb = xexpr_to_anyreg_readonly(ctx, compiler, &str_e);

            XrExprDesc int_e = xr_compile_expr(ctx, compiler, node->left);
            int rc = xexpr_to_anyreg_readonly(ctx, compiler, &int_e);

            int pc = emit_abc(compiler->emitter, OP_STR_REPEAT, 0, rb, rc);
            e.kind = XEXPR_RELOC;
            e.u.pc = pc;
            return e;
        }
    }

    // ===== Optimization 1: constant folding =====
    if ((node->left->type == AST_LITERAL_INT || node->left->type == AST_LITERAL_FLOAT) &&
        (node->right->type == AST_LITERAL_INT || node->right->type == AST_LITERAL_FLOAT)) {

        LiteralNode *left_lit = (LiteralNode *)&node->left->as;
        LiteralNode *right_lit = (LiteralNode *)&node->right->as;

        TokenType op_token;
        switch (type) {
            case AST_BINARY_ADD: op_token = TK_PLUS; break;
            case AST_BINARY_SUB: op_token = TK_MINUS; break;
            case AST_BINARY_MUL: op_token = TK_STAR; break;
            case AST_BINARY_DIV: op_token = TK_SLASH; break;
            case AST_BINARY_MOD: op_token = TK_PERCENT; break;
            default: op_token = TK_EOF; break;
        }

        if (op_token != TK_EOF) {
            XrValue left_val = (node->left->type == AST_LITERAL_INT) ?
                xr_int(left_lit->raw_value.int_val) :
                xr_float(left_lit->raw_value.float_val);

            XrValue right_val = (node->right->type == AST_LITERAL_INT) ?
                xr_int(right_lit->raw_value.int_val) :
                xr_float(right_lit->raw_value.float_val);

            XrValue result;
            if (xr_opt_fold_binary(op_token, left_val, right_val, &result)) {
                // Folding succeeded! Emit LOADK, A=0 pending relocation
                int kidx = xr_vm_proto_add_constant(compiler->proto, result);
                int pc = emit_abx(compiler->emitter, OP_LOADK, 0, kidx);
                e.kind = XEXPR_RELOC;
                e.u.pc = pc;
                e.compile_type = result_ct;
                return e;
            }
        }
    }

    // ===== Const propagation: resolve const variables to literals in-place =====
    // Macro-like expansion: replace AST_VARIABLE with AST_LITERAL_INT/FLOAT
    // so all downstream optimizations (MULI, MULK, commutative swap, fold) work automatically
    for (int side = 0; side < 2; side++) {
        AstNode *op = (side == 0) ? node->left : node->right;
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
                // Also check ConstEntry (top-level / upvalue constants)
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

    // ===== Commutative swap: move constant to right for ADDI/MULI/ADDK/MULK =====
    if ((type == AST_BINARY_ADD || type == AST_BINARY_MUL) &&
        (node->left->type == AST_LITERAL_INT || node->left->type == AST_LITERAL_FLOAT) &&
        (node->right->type != AST_LITERAL_INT && node->right->type != AST_LITERAL_FLOAT)) {
        AstNode *tmp = node->left;
        node->left = node->right;
        node->right = tmp;
    }

    // ===== Optimization 2: immediate operand optimization (small integer: -128~127) =====
    if (node->right->type == AST_LITERAL_INT) {
        LiteralNode *lit = (LiteralNode *)&node->right->as;
        xr_Integer value = lit->raw_value.int_val;

        if (value >= -128 && value <= 127) {
            OpCode op = (OpCode)0;
            bool use_optimized = true;

            // Exclude string multiplication (handled by OP_STR_REPEAT)
            if (type == AST_BINARY_MUL) {
                XrType *left_ct_imm = get_expr_type(ctx, compiler, node->left);
                if (left_ct_imm && left_ct_imm->kind == XR_KIND_STRING) {
                    use_optimized = false;
                }
            }

            switch (type) {
                case AST_BINARY_ADD: op = OP_ADDI; break;
                case AST_BINARY_SUB: op = OP_SUBI; break;
                case AST_BINARY_MUL: op = OP_MULI; break;
                default: use_optimized = false; break;
            }

            if (use_optimized) {
                XrExprDesc left_e = xr_compile_expr(ctx, compiler, node->left);
                int rb = xexpr_to_anyreg_readonly(ctx, compiler, &left_e);

                uint8_t c_val = (uint8_t)((int)value & 0xFF);
                int pc = emit_abc(compiler->emitter, op, 0, rb, c_val);

                e.kind = XEXPR_RELOC;
                e.u.pc = pc;
                e.compile_type = result_ct;
                return e;
            }
        }
    }

    // ===== Optimization 3: constant table optimization (ADDK/SUBK/MULK/DIVK) =====
    // Right operand is numeric constant (float or integer out of immediate range)
    if (node->right->type == AST_LITERAL_INT || node->right->type == AST_LITERAL_FLOAT) {
        OpCode op = (OpCode)0;
        bool use_k_op = true;

        switch (type) {
            case AST_BINARY_ADD: op = OP_ADDK; break;
            case AST_BINARY_SUB: op = OP_SUBK; break;
            case AST_BINARY_MUL: op = OP_MULK; break;
            case AST_BINARY_DIV: op = OP_DIVK; break;
            default: use_k_op = false; break;
        }

        if (use_k_op) {
            // Compile left operand to register
            XrExprDesc left_e = xr_compile_expr(ctx, compiler, node->left);
            int rb = xexpr_to_anyreg_readonly(ctx, compiler, &left_e);

            // Add constant to constant table
            LiteralNode *lit = (LiteralNode *)&node->right->as;
            XrValue kval = (node->right->type == AST_LITERAL_INT) ?
                xr_int(lit->raw_value.int_val) :
                xr_float(lit->raw_value.float_val);
            int kidx = xr_vm_proto_add_constant(compiler->proto, kval);

            // Emit ADDK/SUBK/MULK/DIVK, A=0 pending relocation
            int pc = emit_abc(compiler->emitter, op, 0, rb, kidx);

            e.kind = XEXPR_RELOC;
            e.u.pc = pc;
            e.compile_type = result_ct;
            return e;
        }
    }

    // ===== Generic path: true RELOC implementation =====

    XrType *left_ct = get_expr_type(ctx, compiler, node->left);
    XrType *right_ct = get_expr_type(ctx, compiler, node->right);

    // ===== Strict type checking for + operator =====
    // Rules: same types only, exception: int+float promotes to float
    // string+string → STRBUF sequence, numeric+numeric → OP_ADD
    // string+non-string → compile error, unknown+unknown → OP_ADD (runtime)
    if (type == AST_BINARY_ADD && left_ct && right_ct) {
        bool left_str = XR_TYPE_IS_STRING(left_ct);
        bool right_str = XR_TYPE_IS_STRING(right_ct);
        bool left_num = XR_TYPE_IS_INT(left_ct) || XR_TYPE_IS_FLOAT(left_ct);
        bool right_num = XR_TYPE_IS_INT(right_ct) || XR_TYPE_IS_FLOAT(right_ct);
        bool left_unknown = (left_ct->kind == XR_KIND_UNKNOWN);
        bool right_unknown = (right_ct->kind == XR_KIND_UNKNOWN);

        // string + string → STRBUF sequence (two-operand fallback)
        if (left_str && right_str) {
            int buf_reg = reg_alloc(ctx, compiler);
            emit_abc(compiler->emitter, OP_STRBUF_NEW, buf_reg, 0, 0);

            XrExprDesc left_e = xr_compile_expr(ctx, compiler, node->left);
            int rb_s = xexpr_to_anyreg(ctx, compiler, &left_e);
            emit_abc(compiler->emitter, OP_STRBUF_APPEND, buf_reg, rb_s, 0);
            if (rb_s != buf_reg) reg_free(compiler, rb_s);

            XrExprDesc right_e = xr_compile_expr(ctx, compiler, node->right);
            int rc_s = xexpr_to_anyreg(ctx, compiler, &right_e);
            emit_abc(compiler->emitter, OP_STRBUF_APPEND, buf_reg, rc_s, 0);
            if (rc_s != buf_reg) reg_free(compiler, rc_s);

            emit_abc(compiler->emitter, OP_STRBUF_FINISH, buf_reg, 0, 0);
            xexpr_init(&e, XEXPR_TEMP, buf_reg);
            e.compile_type = (XrType *)xr_type_new_string();
            return e;
        }

        // string + non-string or non-string + string → compile error
        // (except when either side is unknown — that's runtime's problem)
        if ((left_str && !right_str && !right_unknown) ||
            (!left_str && !left_unknown && right_str)) {
            xr_compiler_error(ctx, compiler,
                "operator '+' requires both operands to be the same type; "
                "use \"${expr}\" interpolation or .toString() for string conversion");
            e.kind = XEXPR_VOID;
            return e;
        }

        // numeric + non-numeric (non-unknown, non-string) → compile error
        if ((left_num && !right_num && !right_unknown && !right_str) ||
            (!left_num && !left_unknown && !left_str && right_num)) {
            xr_compiler_error(ctx, compiler,
                "operator '+' requires both operands to be numeric or both string");
            e.kind = XEXPR_VOID;
            return e;
        }
    }

    // Compile operands to registers
    XrExprDesc left_e = xr_compile_expr(ctx, compiler, node->left);
    int rb = xexpr_to_anyreg_readonly(ctx, compiler, &left_e);
    XrExprDesc right_e = xr_compile_expr(ctx, compiler, node->right);
    int rc = xexpr_to_anyreg_readonly(ctx, compiler, &right_e);

    // Ensure tagged format for generic instructions
    rb = ensure_boxed(ctx, compiler, &left_e, rb);
    rc = ensure_boxed(ctx, compiler, &right_e, rc);

    OpCode op;
    switch (type) {
        case AST_BINARY_ADD:    op = OP_ADD; break;
        case AST_BINARY_SUB:    op = OP_SUB; break;
        case AST_BINARY_MUL:    op = OP_MUL; break;
        case AST_BINARY_DIV:    op = OP_DIV; break;
        case AST_BINARY_MOD:    op = OP_MOD; break;
        case AST_BINARY_BAND:   op = OP_BAND; break;
        case AST_BINARY_BOR:    op = OP_BOR; break;
        case AST_BINARY_BXOR:   op = OP_BXOR; break;
        case AST_BINARY_LSHIFT: op = OP_SHL; break;
        case AST_BINARY_RSHIFT: op = OP_SHR; break;

        default:
            xr_log_warning("compiler", "unknown binary operator: %d", type);
            xr_compiler_error(ctx, compiler, "Unknown binary operator");
            e.kind = XEXPR_VOID;
            return e;
    }

    int pc = emit_abc(compiler->emitter, op, 0, rb, rc);

    e.kind = XEXPR_RELOC;
    e.u.pc = pc;
    e.compile_type = result_ct;
    return e;
}

/* ========== Comparison Operations ========== */

/*
 * Internal implementation: compile comparison operation (returns register).
 *
 * Optimization strategy:
 * 1. Constant folding: two constants directly compute result
 * 2. Use immediate comparison when right operand is small integer (e.g. x < 10)
 * 3. Generic path uses normal comparison instruction
 *
 * This function still returns int (register), for Phase 3 migration transition.
 */
static int compile_comparison_internal(XrCompilerContext *ctx, XrCompiler *compiler, BinaryNode *node, AstNodeType type) {
    // ===== Operator overload recursion warning =====
    // Check if using same operator inside operator method
    if (ctx->current_operator != NULL) {
        const char *op_name = NULL;

        // Determine currently used operator
        switch (type) {
            case AST_BINARY_EQ: op_name = "=="; break;
            case AST_BINARY_NE: op_name = "!="; break;
            case AST_BINARY_LT: op_name = "<"; break;
            case AST_BINARY_LE: op_name = "<="; break;
            case AST_BINARY_GT: op_name = ">"; break;
            case AST_BINARY_GE: op_name = ">="; break;
            case AST_BINARY_EQ_STRICT: op_name = "==="; break;
            case AST_BINARY_NE_STRICT: op_name = "!=="; break;
            default: break;
        }

        // If using == inside operator== (but not ===), emit warning
        if (op_name != NULL && strcmp(ctx->current_operator, op_name) == 0) {
            // Using === is safe, no warning needed
            if (type != AST_BINARY_EQ_STRICT && type != AST_BINARY_NE_STRICT) {
                char msg[256];
                snprintf(msg, sizeof(msg),
                    "using %s inside operator%s may cause infinite recursion; "
                    "use %s%s for reference comparison, or ensure recursion is intentional",
                    ctx->current_operator,
                    op_name,
                    op_name,
                    (type == AST_BINARY_EQ || type == AST_BINARY_NE) ? "=" : "");
                xr_diag_print(XR_DIAG_WARNING, 0, msg,
                              ctx->source_file, ctx->current_line,
                              ctx->current_column > 0 ? ctx->current_column : 1,
                              0, NULL, NULL);
            }
        }
    }

    // ===== Optimization 1: constant folding =====
    if ((node->left->type == AST_LITERAL_INT || node->left->type == AST_LITERAL_FLOAT) &&
        (node->right->type == AST_LITERAL_INT || node->right->type == AST_LITERAL_FLOAT)) {

        LiteralNode *left_lit = (LiteralNode *)&node->left->as;
        LiteralNode *right_lit = (LiteralNode *)&node->right->as;

        // Convert to XrValue
        XrValue left_val = (node->left->type == AST_LITERAL_INT) ?
            xr_int(left_lit->raw_value.int_val) :
            xr_float(left_lit->raw_value.float_val);

        XrValue right_val = (node->right->type == AST_LITERAL_INT) ?
            xr_int(right_lit->raw_value.int_val) :
            xr_float(right_lit->raw_value.float_val);

        // Convert AST type to Token type
        TokenType op_token;
        switch (type) {
            case AST_BINARY_EQ: op_token = TK_EQ; break;
            case AST_BINARY_NE: op_token = TK_NE; break;
            case AST_BINARY_EQ_STRICT: op_token = TK_EQ_STRICT; break;
            case AST_BINARY_NE_STRICT: op_token = TK_NE_STRICT; break;
            case AST_BINARY_LT: op_token = TK_LT; break;
            case AST_BINARY_LE: op_token = TK_LE; break;
            case AST_BINARY_GT: op_token = TK_GT; break;
            case AST_BINARY_GE: op_token = TK_GE; break;
            default: op_token = TK_EOF; break;
        }

        // Try constant folding
        XrValue result;
        if (op_token != TK_EOF && xr_opt_fold_comparison(op_token, left_val, right_val, &result)) {
            // Folding succeeded! Load constant
            int dst = reg_alloc(ctx, compiler);
            if (XR_TO_BOOL(result)) {
                emit_loadtrue(compiler->emitter, dst);
            } else {
                emit_loadfalse(compiler->emitter, dst);
            }
            xreg_set_freereg(compiler->regalloc, dst + 1);
            return dst;
        }
    }

    // ===== Optimization 2: null comparison optimization =====
    // Use OP_ISNULL_SET: R[A] = (R[B] == null), single instruction
    if (node->right->type == AST_LITERAL_NULL &&
        (type == AST_BINARY_EQ || type == AST_BINARY_NE ||
         type == AST_BINARY_EQ_STRICT || type == AST_BINARY_NE_STRICT)) {

        // Compile left operand
        XrExprDesc left_e = xr_compile_expr(ctx, compiler, node->left);
        int rb = xexpr_to_anyreg_readonly(ctx, compiler, &left_e);

        // Allocate result register
        int ra = reg_alloc(ctx, compiler);

        // Use OP_ISNULL_SET: R[ra] = (R[rb] == null)
        emit_abc(compiler->emitter, OP_ISNULL_SET, ra, rb, 0);

        // For != null, negate the result
        if (type == AST_BINARY_NE || type == AST_BINARY_NE_STRICT) {
            emit_abc(compiler->emitter, OP_NOT, ra, ra, 0);
        }

        xreg_set_freereg(compiler->regalloc, ra + 1);
        return ra;
    }

    // ===== Optimization 3: immediate comparison =====
    // Optimization: if right operand is small integer constant, use immediate comparison instruction
    if (node->right->type == AST_LITERAL_INT) {
        LiteralNode *lit = (LiteralNode *)&node->right->as;
        xr_Integer value = lit->raw_value.int_val;

        // Small integer range
        if (value >= -128 && value <= 127) {
            XrExprDesc left_e = xr_compile_expr(ctx, compiler, node->left);
            int rb = xexpr_to_anyreg_readonly(ctx, compiler, &left_e);
            // BOX if typed operand (immediate comparisons expect tagged)
            rb = ensure_boxed(ctx, compiler, &left_e, rb);

            // Select comparison instruction
            OpCode op_imm;
            bool negate = false;

            switch (type) {
                case AST_BINARY_EQ:
                case AST_BINARY_EQ_STRICT:
                    op_imm = OP_EQI;
                    break;
                case AST_BINARY_NE:
                case AST_BINARY_NE_STRICT:
                    op_imm = OP_EQI;
                    negate = true;
                    break;
                case AST_BINARY_LT:
                    op_imm = OP_LTI;
                    break;
                case AST_BINARY_LE:
                    op_imm = OP_LEI;
                    break;
                case AST_BINARY_GT:
                    op_imm = OP_LEI;
                    negate = !negate;  // GT = NOT(LE)
                    break;
                case AST_BINARY_GE:
                    op_imm = OP_LTI;
                    negate = !negate;  // GE = NOT(LT)
                    break;
                default:
                    goto general_path;
            }

            // Allocate result register
            int ra = reg_alloc(ctx, compiler);

            // Format: CMP_IMM R[rb] imm 1 (using emit_absc wrapper)
            // ABsC format: A=rb, B=(int)value (full value), sC=1
            uint8_t c_imm = (uint8_t)((int)value & 0xFF);
            emit_abc(compiler->emitter, op_imm, rb, c_imm, 1);

            // Jump when comparison is true
            int true_jump = emit_jump(compiler->emitter, OP_JMP);

            // Comparison is false: load corresponding value
            if (negate) {
                emit_loadtrue(compiler->emitter, ra);
            } else {
                emit_loadfalse(compiler->emitter, ra);
            }

            // Skip true branch
            int end_jump = emit_jump(compiler->emitter, OP_JMP);

            // Patch true jump
            patch_jump(compiler->emitter, true_jump, -1);

            // Comparison is true: load corresponding value
            if (negate) {
                emit_loadfalse(compiler->emitter, ra);
            } else {
                emit_loadtrue(compiler->emitter, ra);
            }

            // Patch end jump
            patch_jump(compiler->emitter, end_jump, -1);

            // Set freereg = ra + 1, reclaim rb
            xreg_set_freereg(compiler->regalloc, ra + 1);

            return ra;
        }
    }

general_path:
    // Generic path: use OP_CMP_* instruction to directly generate Bool value
    {
        XrExprDesc left_e = xr_compile_expr(ctx, compiler, node->left);
        int rb = xexpr_to_anyreg_readonly(ctx, compiler, &left_e);
        XrExprDesc right_e = xr_compile_expr(ctx, compiler, node->right);
        int rc = xexpr_to_anyreg_readonly(ctx, compiler, &right_e);

        // Ensure tagged format (OP_CMP_* expect tagged)
        rb = ensure_boxed(ctx, compiler, &left_e, rb);
        rc = ensure_boxed(ctx, compiler, &right_e, rc);

        OpCode op;
        switch (type) {
            case AST_BINARY_EQ: op = OP_CMP_EQ; break;
            case AST_BINARY_NE: op = OP_CMP_NE; break;
            case AST_BINARY_EQ_STRICT: op = OP_CMP_EQ_STRICT; break;
            case AST_BINARY_NE_STRICT: op = OP_CMP_NE_STRICT; break;
            case AST_BINARY_LT: op = OP_CMP_LT; break;
            case AST_BINARY_LE: op = OP_CMP_LE; break;
            case AST_BINARY_GT: op = OP_CMP_LT; { int tmp = rb; rb = rc; rc = tmp; } break;
            case AST_BINARY_GE: op = OP_CMP_LE; { int tmp = rb; rb = rc; rc = tmp; } break;

            default:
                xr_log_warning("compiler", "unknown comparison operator: %d", type);
                xr_compiler_error(ctx, compiler, "Unknown comparison operator");
                return -1;
        }

        int ra = reg_alloc(ctx, compiler);
        emit_abc(compiler->emitter, op, ra, rb, rc);
        xreg_set_freereg(compiler->regalloc, ra + 1);

        return ra;
    }
}

/*
 * Compile comparison operation (returns XrExprDesc)
 */
XrExprDesc compile_comparison(XrCompilerContext *ctx, XrCompiler *compiler, BinaryNode *node, AstNodeType type) {
    XrExprDesc e = {0};
    int reg = compile_comparison_internal(ctx, compiler, node, type);
    xexpr_init(&e, XEXPR_TEMP, reg);
    e.compile_type = xr_type_new_bool();
    return e;
}

/*
 * Compile 'is' expression for runtime type checking
 * Generates: R[dest] = (R[src] is Type) ? true : false
 */
XrExprDesc compile_is_expr(XrCompilerContext *ctx, XrCompiler *compiler, IsExprNode *node) {
    XrExprDesc e = {0};

    // Compile the expression to check
    XrExprDesc expr = xr_compile_expr(ctx, compiler, node->expr);
    int src_reg = xexpr_to_anyreg(ctx, compiler, &expr);

    // Get the type kind as XrTypeId constant
    int type_kind = -1;  // -1 means unknown type (always false)
    if (node->type) {
        XrType *t = (XrType*)node->type;
        if (t->kind == XR_KIND_INT) type_kind = XR_TID_INT;
        else if (t->kind == XR_KIND_FLOAT) type_kind = XR_TID_FLOAT;
        else if (t->kind == XR_KIND_STRING) type_kind = XR_TID_STRING;
        else if (t->kind == XR_KIND_BOOL) type_kind = XR_TID_BOOL;
        else if (t->kind == XR_KIND_NULL) type_kind = XR_TID_NULL;
        else if (t->kind == XR_KIND_ARRAY) type_kind = XR_TID_ARRAY;
        else if (t->kind == XR_KIND_MAP) type_kind = XR_TID_MAP;
        else if (t->kind == XR_KIND_SET) type_kind = XR_TID_SET;
        else if (t->kind == XR_KIND_CLASS || t->kind == XR_KIND_INSTANCE) type_kind = XR_TID_INSTANCE;
        else if (t->kind == XR_KIND_FUNCTION) type_kind = XR_TID_FUNCTION;
    }

    // Add type constant
    int type_const = xr_vm_proto_add_constant(compiler->proto, xr_int(type_kind));

    // Allocate destination register
    int dest_reg = reg_alloc(ctx, compiler);

    // Emit OP_IS instruction
    emit_abc(compiler->emitter, OP_IS, dest_reg, src_reg, type_const);

    // Free source register
    reg_free(compiler, src_reg);

    xexpr_init(&e, XEXPR_TEMP, dest_reg);
    return e;
}

/* ========== Ternary Expression ========== */

/*
 * Internal implementation: compile ternary expression (returns register)
 */
static int compile_ternary_internal(XrCompilerContext *ctx, XrCompiler *compiler, TernaryNode *node) {
    // Compile condition expression
    XrExprDesc cond_expr = xr_compile_expr(ctx, compiler, node->condition);
    int cond_reg = xexpr_to_anyreg(ctx, compiler, &cond_expr);

    // Test condition, if false skip true branch
    emit_abc(compiler->emitter, OP_TEST, cond_reg, 0, 0);
    int else_jump = emit_jump(compiler->emitter, OP_JMP);
    reg_free(compiler, cond_reg);

    // Compile true expression
    XrExprDesc true_expr = xr_compile_expr(ctx, compiler, node->true_expr);
    int true_reg = xexpr_to_anyreg(ctx, compiler, &true_expr);

    // Allocate result register
    int result_reg = reg_alloc(ctx, compiler);
    emit_move(compiler->emitter, result_reg, true_reg);
    reg_free(compiler, true_reg);

    // Skip else branch
    int end_jump = emit_jump(compiler->emitter, OP_JMP);

    // Patch else jump
    patch_jump(compiler->emitter, else_jump, -1);

    // Compile false expression
    XrExprDesc false_expr = xr_compile_expr(ctx, compiler, node->false_expr);
    int false_reg = xexpr_to_anyreg(ctx, compiler, &false_expr);

    // Move result to result register
    emit_move(compiler->emitter, result_reg, false_reg);
    reg_free(compiler, false_reg);

    // Patch end jump
    patch_jump(compiler->emitter, end_jump, -1);

    return result_reg;
}

/*
 * Compile ternary expression (returns XrExprDesc)
 */
XrExprDesc compile_ternary(XrCompilerContext *ctx, XrCompiler *compiler, TernaryNode *node) {
    XrExprDesc e = {0};
    int reg = compile_ternary_internal(ctx, compiler, node);
    xexpr_init(&e, XEXPR_TEMP, reg);
    return e;
}


/* ========== Nullish Coalescing ========== */

/*
 * Internal implementation: compile nullish coalescing (returns register)
 *
 * Optimization: If compile-time type analysis proves left operand is non-nullable,
 * skip the null check entirely and just return the left operand.
 */
static int compile_nullish_coalesce_internal(XrCompilerContext *ctx, XrCompiler *compiler, BinaryNode *node) {
    // Skip null check when left operand is provably non-null at compile time.
    // Case 1: Literal values (int, float, string, true, false) are never null
    AstNodeType ltype = node->left->type;
    if (ltype == AST_LITERAL_INT || ltype == AST_LITERAL_FLOAT ||
        ltype == AST_LITERAL_STRING || ltype == AST_LITERAL_TRUE ||
        ltype == AST_LITERAL_FALSE) {
        XrExprDesc left_expr = xr_compile_expr(ctx, compiler, node->left);
        return xexpr_to_anyreg(ctx, compiler, &left_expr);
    }
    // Case 2: Analyzer compile_type is non-nullable and concrete (not unknown).
    // Covers narrowed variables, const declarations, non-nullable function returns.
    if (node->left->compile_type &&
        !node->left->compile_type->is_nullable &&
        node->left->compile_type->kind != XR_KIND_UNKNOWN &&
        node->left->compile_type->kind != XR_KIND_NULL) {
        XrExprDesc left_expr = xr_compile_expr(ctx, compiler, node->left);
        return xexpr_to_anyreg(ctx, compiler, &left_expr);
    }

    // Compile left operand
    XrExprDesc left_expr = xr_compile_expr(ctx, compiler, node->left);
    int left_reg = xexpr_to_anyreg(ctx, compiler, &left_expr);

    /* Check if null.
     * OP_EQK A B k: if (R[A] == K[B]) != k then PC++
     * When k=1:
     *   - left == null: (1 != 1) = false, don't skip, execute JMP to default value
     *   - left != null: (0 != 1) = true, skip JMP, use left operand
     */
    XrValue null_val = xr_null();
    int null_const = xr_vm_proto_add_constant(compiler->proto, null_val);
    emit_abc(compiler->emitter, OP_EQK, left_reg, null_const, 1);
    int use_default_jump = emit_jump(compiler->emitter, OP_JMP);

    // Left operand is not null, use left operand
    int result_reg = reg_alloc(ctx, compiler);
    emit_move(compiler->emitter, result_reg, left_reg);
    reg_free(compiler, left_reg);

    int end_jump = emit_jump(compiler->emitter, OP_JMP);

    // Patch use_default jump
    patch_jump(compiler->emitter, use_default_jump, -1);

    // Compile right operand (default value)
    XrExprDesc right_expr = xr_compile_expr(ctx, compiler, node->right);
    int right_reg = xexpr_to_anyreg(ctx, compiler, &right_expr);

    // Move result to result register
    emit_move(compiler->emitter, result_reg, right_reg);
    reg_free(compiler, right_reg);

    // Patch end jump
    patch_jump(compiler->emitter, end_jump, -1);

    return result_reg;
}

/*
 * Compile nullish coalescing (returns XrExprDesc)
 */
XrExprDesc compile_nullish_coalesce(XrCompilerContext *ctx, XrCompiler *compiler, BinaryNode *node) {
    XrExprDesc e = {0};
    int reg = compile_nullish_coalesce_internal(ctx, compiler, node);
    xexpr_init(&e, XEXPR_TEMP, reg);
    return e;
}


/* ========== Optional Chaining ========== */

/*
 * Internal implementation: compile optional chaining (returns register)
 *
 * Optimization: If compile-time type analysis proves object is non-nullable,
 * skip the null check and generate direct property/index access.
 */
static int compile_optional_chain_internal(XrCompilerContext *ctx, XrCompiler *compiler, OptionalChainNode *node) {
    // Only skip null check for literal values known non-null at compile time
    // Variable types may be nullable even if compile_type doesn't have is_nullable set
    AstNodeType otype = node->object->type;
    if (otype == AST_LITERAL_INT || otype == AST_LITERAL_FLOAT ||
        otype == AST_LITERAL_STRING || otype == AST_LITERAL_TRUE ||
        otype == AST_LITERAL_FALSE) {
        XrExprDesc obj_expr = xr_compile_expr(ctx, compiler, node->object);
        int obj_reg = xexpr_to_anyreg(ctx, compiler, &obj_expr);
        int result_reg = reg_alloc(ctx, compiler);

        if (node->chain_type == 0) {
            int global_sym = xr_symbol_register_in_table(
                (XrSymbolTable*)xr_isolate_get_symbol_table(ctx->X), node->name);
            int local_sym = emitter_add_symbol(compiler->emitter, global_sym);
            emit_abc(compiler->emitter, OP_GETPROP, result_reg, obj_reg, local_sym);
        } else if (node->chain_type == 1) {
            XrExprDesc index_expr = xr_compile_expr(ctx, compiler, node->index);
            int index_reg = xexpr_to_anyreg(ctx, compiler, &index_expr);
            emit_abc(compiler->emitter, OP_INDEX_GET, result_reg, obj_reg, index_reg);
            reg_free(compiler, index_reg);
        }

        xreg_set_freereg(compiler->regalloc, result_reg + 1);
        return result_reg;
    }

    // Compile object expression
    XrExprDesc obj_expr = xr_compile_expr(ctx, compiler, node->object);
    int obj_reg = xexpr_to_anyreg(ctx, compiler, &obj_expr);

    /* Check if object is null.
     * OP_EQK A B k: if (R[A] == K[B]) != k then PC++
     * When k=1:
     *   - obj == null: (1 != 1) = false, don't skip, execute JMP return null
     *   - obj != null: (0 != 1) = true, skip JMP, execute property access
     */
    XrValue null_val = xr_null();
    int null_const = xr_vm_proto_add_constant(compiler->proto, null_val);
    emit_abc(compiler->emitter, OP_EQK, obj_reg, null_const, 1);
    int return_null_jump = emit_jump(compiler->emitter, OP_JMP);

    // Object is not null, execute access
    int result_reg = reg_alloc(ctx, compiler);

    if (node->chain_type == 0) {
        /* Property access: obj?.prop
         * Note: OP_GETPROP's C parameter is symbol, not constant index
         */
        int global_sym = xr_symbol_register_in_table(
            (XrSymbolTable*)xr_isolate_get_symbol_table(ctx->X), node->name);
        int local_sym = emitter_add_symbol(compiler->emitter, global_sym);
        emit_abc(compiler->emitter, OP_GETPROP, result_reg, obj_reg, local_sym);
    } else if (node->chain_type == 1) {
        // Index access: obj?.[index]
        XrExprDesc index_expr = xr_compile_expr(ctx, compiler, node->index);
        int index_reg = xexpr_to_anyreg(ctx, compiler, &index_expr);
        emit_abc(compiler->emitter, OP_INDEX_GET, result_reg, obj_reg, index_reg);
        reg_free(compiler, index_reg);
    } else {
        // Method call: obj?.method() - should be handled in call resolution, error here
        xr_log_warning("compiler", "optional chain method call not yet implemented");
        xr_compiler_error(ctx, compiler, "Optional chain method call not yet implemented");
        return result_reg;
    }

    xreg_set_freereg(compiler->regalloc, result_reg + 1);

    int end_jump = emit_jump(compiler->emitter, OP_JMP);

    // Patch return_null jump
    patch_jump(compiler->emitter, return_null_jump, -1);

    // Return null
    emit_loadnull(compiler->emitter, result_reg);

    // Patch end jump
    patch_jump(compiler->emitter, end_jump, -1);

    return result_reg;
}

/*
 * Compile optional chaining (returns XrExprDesc)
 */
XrExprDesc compile_optional_chain(XrCompilerContext *ctx, XrCompiler *compiler, OptionalChainNode *node) {
    XrExprDesc e = {0};
    int reg = compile_optional_chain_internal(ctx, compiler, node);
    xexpr_init(&e, XEXPR_TEMP, reg);
    return e;
}

/*
 * Compile force unwrap: expr! — panics at runtime if value is null.
 *
 * Generated code:
 *   eval operand -> R[val]
 *   if R[val] != null: skip JMP
 *   JMP to panic
 *   [panic]: OP_THROW with "Force unwrap of null value"
 *   [ok]: result = R[val]
 */
int compile_force_unwrap(XrCompilerContext *ctx, XrCompiler *compiler, UnaryNode *node) {
    // Evaluate operand
    XrExprDesc val_expr = xr_compile_expr(ctx, compiler, node->operand);
    int val_reg = xexpr_to_anyreg(ctx, compiler, &val_expr);

    // OP_EQK A B k: if (R[A] == K[B]) != k then PC++ (skip next instruction)
    // k=1: val == null -> (1!=1)=false -> don't skip JMP -> jump to ok (wrong for panic)
    // We want: val == null -> panic, val != null -> continue
    // Use k=0: val == null -> (1!=0)=true -> skip JMP -> fall into panic
    //          val != null -> (0!=0)=false -> don't skip JMP -> jump to ok
    XrValue null_val = xr_null();
    int null_const = xr_vm_proto_add_constant(compiler->proto, null_val);
    emit_abc(compiler->emitter, OP_EQK, val_reg, null_const, 0);
    int ok_jump = emit_jump(compiler->emitter, OP_JMP);

    // Panic path (val == null): load error message and throw
    const char *msg = "Force unwrap of null value";
    XrString *msg_str = xr_compile_time_intern(ctx->X, msg, (int)strlen(msg));
    XrValue msg_val = xr_string_value(msg_str);
    int msg_const = xr_vm_proto_add_constant(compiler->proto, msg_val);
    int msg_reg = reg_alloc(ctx, compiler);
    emit_abx(compiler->emitter, OP_LOADK, msg_reg, msg_const);
    emit_abc(compiler->emitter, OP_THROW, msg_reg, 0, 0);
    reg_free(compiler, msg_reg);

    // Ok path (val != null): patch ok_jump to here
    patch_jump(compiler->emitter, ok_jump, -1);

    return val_reg;
}

/*
 * Map compile-time XrType flags to XrTypeId for runtime typeof check.
 * Returns -1 if the type is not a primitive/known type.
 */
static int xrtype_to_typeid(XrType *type) {
    if (!type) return -1;
    switch (type->kind) {
    case XR_KIND_INT:    return XR_TID_INT;
    case XR_KIND_FLOAT:  return XR_TID_FLOAT;
    case XR_KIND_STRING: return XR_TID_STRING;
    case XR_KIND_BOOL:   return XR_TID_BOOL;
    case XR_KIND_JSON:   return XR_TID_JSON;
    case XR_KIND_ARRAY:  return XR_TID_ARRAY;
    default:             return -1;
    }
}

/*
 * Compile force unwrap: expr as TargetType / expr as TargetType?
 *
 * Non-safe (expr as T):
 *   typeof(val) == expected_tid  -> ok, return val
 *   else -> OP_THROW "Type cast failed: expected T"
 *
 * Safe (expr as T?):
 *   typeof(val) == expected_tid  -> ok, return val
 *   else -> OP_LOADNULL, return null
 */
int compile_as_expr(XrCompilerContext *ctx, XrCompiler *compiler, AsExprNode *node) {
    XrType *target = node->type;
    int tid = target ? xrtype_to_typeid(target) : -1;

    // Evaluate operand
    XrExprDesc val_expr = xr_compile_expr(ctx, compiler, node->expr);
    int val_reg = xexpr_to_anyreg(ctx, compiler, &val_expr);

    // If target type is unknown, just return the value as-is
    if (!target || XR_TYPE_IS_UNKNOWN(target) || tid < 0) {
        return val_reg;
    }

    // Allocate type_reg for typeof result
    int type_reg = reg_alloc(ctx, compiler);
    emit_abc(compiler->emitter, OP_TYPEOF, type_reg, val_reg, 0);

    // OP_EQK type_reg, tid_const, 1:
    //   type == tid: (1!=1)=false -> don't skip JMP -> jump to ok
    //   type != tid: (0!=1)=true -> skip JMP -> fall into error path
    XrValue tid_val = xr_int(tid);
    int tid_const = xr_vm_proto_add_constant(compiler->proto, tid_val);
    emit_abc(compiler->emitter, OP_EQK, type_reg, tid_const, 1);
    int ok_jump = emit_jump(compiler->emitter, OP_JMP);
    reg_free(compiler, type_reg);

    if (node->is_safe) {
        // Safe cast: type mismatch -> load null, then jump to after ok path
        // Layout: [LOADNULL] [JMP -> after_ok] [ok path]
        emit_abc(compiler->emitter, OP_LOADNULL, val_reg, 0, 0);
        int end_jump = emit_jump(compiler->emitter, OP_JMP);
        // Ok path: val stays as-is
        patch_jump(compiler->emitter, ok_jump, -1);
        patch_jump(compiler->emitter, end_jump, -1);
    } else {
        // Non-safe: type mismatch -> throw TypeError (no JMP needed, throw is terminal)
        const char *type_name = xr_type_to_string(target);
        char err_buf[128];
        snprintf(err_buf, sizeof(err_buf), "Type cast failed: expected %s", type_name);
        XrString *err_str = xr_compile_time_intern(ctx->X, err_buf, (int)strlen(err_buf));
        XrValue err_val = xr_string_value(err_str);
        int err_const = xr_vm_proto_add_constant(compiler->proto, err_val);
        int err_reg = reg_alloc(ctx, compiler);
        emit_abx(compiler->emitter, OP_LOADK, err_reg, err_const);
        emit_abc(compiler->emitter, OP_THROW, err_reg, 0, 0);
        reg_free(compiler, err_reg);
        // Ok path: val stays as-is (ok_jump skips the throw)
        patch_jump(compiler->emitter, ok_jump, -1);
    }

    return val_reg;
}


