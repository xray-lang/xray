/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xexpr_unary.c - Xray unary expression compilation
 *
 * KEY CONCEPT:
 *   - Negation: -x
 *   - Logical NOT: !x
 *
 * OPTIMIZATION:
 *   - Constant folding (compile-time calculation)
 */

#include "xexpr.h"
#include "../../base/xchecks.h"
#include "xcompiler.h"
#include "xcompiler_context.h"
#include "xemit.h"
#include "xoptimize.h"  // constant folding
#include "xexpr_desc.h"
#include "../../runtime/value/xtype.h"
#include <stdio.h>

// ========== Unary Expression Compilation ==========

/*
 * Internal implementation: compile unary expression (returns register)
 *
 * Optimization strategy:
 * - If operand is literal, calculate result at compile time (constant folding)
 * - Otherwise generate runtime instruction
 */
static int compile_unary_internal(XrCompilerContext *ctx, XrCompiler *compiler, UnaryNode *node, AstNodeType type, XrType **out_compile_type) {
    XR_DCHECK(ctx != NULL, "compile_unary: NULL ctx");
    XR_DCHECK(compiler != NULL, "compile_unary: NULL compiler");
    // ===== Constant Folding Optimization =====
    if (node->operand->type == AST_LITERAL_INT ||
        node->operand->type == AST_LITERAL_FLOAT ||
        node->operand->type == AST_LITERAL_TRUE ||
        node->operand->type == AST_LITERAL_FALSE ||
        node->operand->type == AST_LITERAL_NULL) {

        LiteralNode *lit = (LiteralNode *)&node->operand->as;

        // Map AST node type to Token type
        TokenType op_token;
        switch (type) {
            case AST_UNARY_NEG:  op_token = TK_MINUS; break;
            case AST_UNARY_NOT:  op_token = TK_NOT; break;
            case AST_UNARY_BNOT: op_token = TK_TILDE; break;
            default: op_token = TK_EOF; break;
        }

        // Constant folding
        if (op_token != TK_EOF) {
            // Convert LiteralNode to XrValue
            XrValue operand_val = xr_null();
            switch (node->operand->type) {
                case AST_LITERAL_INT:
                    operand_val = xr_int(lit->raw_value.int_val);
                    break;
                case AST_LITERAL_FLOAT:
                    operand_val = xr_float(lit->raw_value.float_val);
                    break;
                case AST_LITERAL_TRUE:
                    operand_val = xr_bool(true);
                    break;
                case AST_LITERAL_FALSE:
                    operand_val = xr_bool(false);
                    break;
                case AST_LITERAL_NULL:
                    operand_val = xr_null();
                    break;
                default:
                    break;  // Don't fold, use general path
            }

            // Try constant folding
            XrValue result;
            if (node->operand->type >= AST_LITERAL_INT && node->operand->type <= AST_LITERAL_NULL &&
                xr_opt_fold_unary(op_token, operand_val, &result)) {
                // Folding successful! Load constant
                int dst = reg_alloc(ctx, compiler);
                int kidx = xr_vm_proto_add_constant(compiler->proto, result);
                emit_abx(compiler->emitter, OP_LOADK, dst, kidx);
                xreg_set_freereg(compiler->regalloc, dst + 1);
                return dst;
            }
        }
    }

    // ===== General Path =====
    XrExprDesc operand_expr = xr_compile_expr(ctx, compiler, node->operand);
    int rb = xexpr_to_anyreg(ctx, compiler, &operand_expr);
    int ra = reg_alloc(ctx, compiler);

    OpCode op;
    XrType *result_type = NULL;
    switch (type) {
        case AST_UNARY_NEG:  op = OP_UNM; break;
        case AST_UNARY_NOT:  op = OP_NOT; break;
        case AST_UNARY_BNOT: op = OP_BNOT; break;
        default:
            xr_compiler_error(ctx, compiler, "unknown unary operator: %d", type);
            return ra;
    }

    // Emit instruction
    emit_abc(compiler->emitter, op, ra, rb, 0);

    // Set freereg = ra + 1, reclaim rb
    xreg_set_freereg(compiler->regalloc, ra + 1);

    if (out_compile_type) *out_compile_type = result_type;
    return ra;
}

/*
 * Compile unary expression (returns XrExprDesc)
 *
 * Note: Returns TEMP instead of RELOC for now
 * Reason: RELOC requires more complex register lifetime management
 */
XrExprDesc compile_unary(XrCompilerContext *ctx, XrCompiler *compiler, UnaryNode *node, AstNodeType type) {
    XR_DCHECK(ctx != NULL, "compile_unary: NULL ctx");
    XR_DCHECK(compiler != NULL, "compile_unary: NULL compiler");
    XrExprDesc e = {0};
    xexpr_init_void(&e);

    XrType *ct = NULL;
    int reg = compile_unary_internal(ctx, compiler, node, type, &ct);
    xexpr_init(&e, XEXPR_TEMP, reg);
    e.compile_type = ct;

    // Infer compile_type: !x→bool, -x→operand type, ~x→int
    if (type == AST_UNARY_NOT) {
        e.compile_type = xr_type_new_bool();
    } else if (type == AST_UNARY_BNOT) {
        e.compile_type = xr_type_new_int();
    } else if (type == AST_UNARY_NEG) {
        XrType *op_ct = get_expr_type(ctx, compiler, node->operand);
        if (op_ct && (XR_TYPE_IS_INT(op_ct) || XR_TYPE_IS_FLOAT(op_ct))) {
            e.compile_type = op_ct;
        }
    }
    return e;
}


