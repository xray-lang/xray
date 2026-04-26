/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xstmt_control.c - If/else and return statement compilation
 *
 * KEY CONCEPT:
 *   Compiles if/else conditional and return statements.
 *   Loop compilation moved to xstmt_loop.c and xstmt_forin.c.
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
#include "xcompiler_class_registry.h"
#include "../analyzer/xanalyzer.h"

/*
 * Emit OP_CHECKTYPE before return if returning Json/JsonValue where
 * primitive or union is declared.  Uses bitmask encoding.
 */
static int kind_to_tid_ret(XrTypeKind kind) {
    switch (kind) {
    case XR_KIND_INT:    return XR_TID_INT;
    case XR_KIND_FLOAT:  return XR_TID_FLOAT;
    case XR_KIND_STRING: return XR_TID_STRING;
    case XR_KIND_BOOL:   return XR_TID_BOOL;
    case XR_KIND_JSON:   return XR_TID_JSON;
    case XR_KIND_NULL:   return XR_TID_NULL;
    default:             return -1;
    }
}

static int64_t build_ret_checktype_mask(XrType *target) {
    if (!target) return 0;
    int tid = kind_to_tid_ret(target->kind);
    if (tid >= 0) return (1LL << tid);
    if (target->kind == XR_KIND_UNION) {
        int64_t mask = 0;
        for (int i = 0; i < target->union_type.member_count; i++) {
            int mt = kind_to_tid_ret(target->union_type.members[i]->kind);
            if (mt < 0) return 0;
            mask |= (1LL << mt);
        }
        if (target->is_nullable) mask |= (1LL << XR_TID_NULL);
        return mask;
    }
    return 0;
}

static void emit_ret_json_checktype(XrCompilerContext *ctx, XrCompiler *compiler,
                                     int reg, AstNode *value_expr) {
    XrType *ret_type = compiler->declared_return_type;
    if (!ret_type || !value_expr) return;
    XrType *expr_type = ctx->analyzer
        ? xa_analyzer_infer_expr_type(ctx->analyzer, value_expr) : NULL;
    if (!expr_type || !xr_is_json_coercion(ret_type, expr_type)) return;
    int64_t mask = build_ret_checktype_mask(ret_type);
    if (mask != 0) {
        int tc = xr_vm_proto_add_constant(compiler->proto, xr_int(mask));
        xemit_checktype(compiler->emitter, reg, tc);
    }
}

/* ========== Return Statement ========== */

/*
 * Compile return statement.
 *
 * Supports multi-value return: return a, b, c
 *
 * Handling:
 * - No return value: return
 * - Single value return: return expr (preserve tail call optimization)
 * - Multi-value return: return a, b, c (consecutive registers)
 */
void compile_return(XrCompilerContext *ctx, XrCompiler *compiler, ReturnStmtNode *node) {
    XR_DCHECK(ctx != NULL, "compile_return: NULL ctx");
    XR_DCHECK(compiler != NULL, "compile_return: NULL compiler");
    if (compiler->type == FUNCTION_SCRIPT) {
        xr_compiler_error(ctx, compiler, "Cannot return from top-level code");
        return;
    }

    int nret = node->value_count;

    // No return value
    if (nret == 0) {
        emit_return(compiler->emitter, 0, 0);
        return;
    }

    // Single value return: preserve tail call optimization
    if (nret == 1) {
        AstNode *value = node->values[0];

        // Detect tail call: if return expression is function call, this is tail position
        if (value->type == AST_CALL_EXPR) {
            CallExprNode *call = (CallExprNode *)&value->as;

            // Method calls: enable tail call for user-defined class methods.
            // Safe patterns: this.method() and ClassName.staticMethod().
            // Builtin type methods (String, Array, etc.) are NOT in class_registry,
            // so they correctly stay excluded (their primitive handlers vmbreak
            // without implicit return).
            if (call->callee->type == AST_MEMBER_ACCESS) {
                MemberAccessNode *ma = &call->callee->as.member_access;
                bool safe_tail = false;
                if (compiler->type == FUNCTION_FUNCTION) {
                    // this.method() — 'this' is AST_THIS_EXPR, not AST_VARIABLE
                    if (ma->object->type == AST_THIS_EXPR) {
                        safe_tail = true;
                    }
                    // ClassName.staticMethod() — user-defined class static method
                    else if (ma->object->type == AST_VARIABLE && ctx->class_registry &&
                             xr_class_registry_is_class(ctx->class_registry,
                                                        ma->object->as.variable.name)) {
                        safe_tail = true;
                    }
                }
                if (safe_tail) {
                    compile_call_internal(ctx, compiler, call, true, NULL);
                } else {
                    int reg = compile_call_internal(ctx, compiler, call, false, NULL);
                    emit_return(compiler->emitter, reg, 1);
                    reg_free(compiler, reg);
                }
            } else if (call->callee->type == AST_VARIABLE) {
                // Variable call: enable tail call unless it's a class constructor.
                // Class constructors (e.g. return User()) must not use tail call
                // because they go through class instantiation, not function call.
                VariableNode *var = &call->callee->as.variable;
                bool is_class_call = (ctx->class_registry &&
                                      xr_class_registry_is_class(ctx->class_registry, var->name));
                if (!is_class_call && compiler->type == FUNCTION_FUNCTION) {
                    // Safe tail call: self-recursive → OP_LOOP_BACK, others → OP_TAILCALL
                    compile_call_internal(ctx, compiler, call, true, NULL);
                } else {
                    int reg = compile_call_internal(ctx, compiler, call, false, NULL);
                    emit_return(compiler->emitter, reg, 1);
                    reg_free(compiler, reg);
                }
            } else {
                // Other calls: tail call optimization
                compile_call_internal(ctx, compiler, call, true, NULL);
                // TAILCALL instruction already contains return semantics, no additional RETURN needed
            }
        } else {
            // Normal single value return
            XrExprDesc expr = xr_compile_expr(ctx, compiler, value);
            int reg = xexpr_to_anyreg(ctx, compiler, &expr);
            emit_ret_json_checktype(ctx, compiler, reg, value);
            emit_return(compiler->emitter, reg, 1);
            reg_free(compiler, reg);
        }
        return;
    }

    // Multi-value return: put all return values into consecutive registers
    int base_reg = xreg_alloc_temp(compiler->regalloc);

    // Protect consecutive register region to prevent reuse when compiling subsequent expressions
    int protect_id = xreg_protect_begin(compiler->regalloc, base_reg, nret, "multi_return");

    // Compile first return value to base_reg
    XrExprDesc expr0 = xr_compile_expr(ctx, compiler, node->values[0]);
    int reg0 = xexpr_to_anyreg(ctx, compiler, &expr0);
    if (reg0 != base_reg) {
        emit_move(compiler->emitter, base_reg, reg0);
        reg_free(compiler, reg0);
    }

    // Compile subsequent return values to consecutive registers
    for (int i = 1; i < nret; i++) {
        int target_reg = base_reg + i;
        // Ensure allocated registers are consecutive
        xreg_alloc_temp(compiler->regalloc);

        XrExprDesc expr = xr_compile_expr(ctx, compiler, node->values[i]);
        int reg = xexpr_to_anyreg(ctx, compiler, &expr);
        if (reg != target_reg) {
            emit_move(compiler->emitter, target_reg, reg);
            reg_free(compiler, reg);
        }
    }

    xreg_protect_end(compiler->regalloc, protect_id);

    // Emit multi-value return instruction
    emit_return(compiler->emitter, base_reg, nret);

    // Release consecutive registers
    for (int i = 0; i < nret; i++) {
        xreg_free_temp(compiler->regalloc, base_reg + i);
    }
}

/* ========== If Statement ========== */

/*
 * Compile if statement.
 *
 * Optimization:
 * - If condition is comparison expression, generate conditional jump directly
 * - Avoid generating intermediate boolean value
 *
 * Generated code (optimized path):
 *   CMP/CMPI <condition>
 *   JMP else_branch
 *   <then_branch>
 *   JMP end
 * else_branch:
 *   <else_branch>
 * end:
 */
void compile_if(XrCompilerContext *ctx, XrCompiler *compiler, IfStmtNode *node) {
    XR_DCHECK(ctx != NULL, "compile_if: NULL ctx");
    XR_DCHECK(compiler != NULL, "compile_if: NULL compiler");
    XR_DCHECK(node != NULL, "compile_if: NULL node");
    // Optimization: directly compile comparison expression to conditional jump
    AstNodeType cond_type = node->condition->type;

    if (cond_type == AST_BINARY_LE || cond_type == AST_BINARY_LT ||
        cond_type == AST_BINARY_GT || cond_type == AST_BINARY_GE ||
        cond_type == AST_BINARY_EQ || cond_type == AST_BINARY_NE) {

        // Directly compile comparison and conditional jump
        BinaryNode *cmp = (BinaryNode *)&node->condition->as;

        int rb = 0, rc = -1;
        OpCode op = OP_LT;
        int k = 0;
        bool use_immediate = false;
        int imm_value = 0;

        // Optimization: check if right operand is small integer constant
        if (cmp->right->type == AST_LITERAL_INT) {
            LiteralNode *lit = (LiteralNode *)&cmp->right->as;
            xr_Integer value = XR_TO_INT(xr_int(lit->raw_value.int_val));

            if (value >= -128 && value <= 127) {
                XrExprDesc left_expr = xr_compile_expr(ctx, compiler, cmp->left);
                rb = xexpr_to_anyreg_readonly(ctx, compiler, &left_expr);
                rb = xexpr_ensure_boxed(ctx, compiler, &left_expr, rb);
                use_immediate = true;
                imm_value = (int)value;

                switch (cond_type) {
                    case AST_BINARY_EQ: op = OP_EQI; break;
                    case AST_BINARY_NE: op = OP_EQI; k = 1; break;
                    case AST_BINARY_LT: op = OP_LTI; break;
                    case AST_BINARY_LE: op = OP_LEI; break;
                    case AST_BINARY_GT: op = OP_LEI; k = !k; break;
                    case AST_BINARY_GE: op = OP_LTI; k = !k; break;
                    default: op = OP_LT; use_immediate = false; break;
                }
            }
        }

        // null comparison optimization: use OP_ISNULL to avoid constant loading
        if (!use_immediate && cmp->right->type == AST_LITERAL_NULL &&
            (cond_type == AST_BINARY_EQ || cond_type == AST_BINARY_NE)) {

            XrExprDesc left_expr = xr_compile_expr(ctx, compiler, cmp->left);
            rb = xexpr_to_anyreg_readonly(ctx, compiler, &left_expr);

            /* OP_ISNULL A k: if (R[A] == null) != k then PC++ (skip next JMP)
             *
             * if (x == null): enter then when x is null
             *   - k=0: if (is_null) != 0 then skip JMP (enter then), else JMP to else
             *
             * if (x != null): enter then when x is not null
             *   - k=1: if (is_null) != 1 then skip JMP (enter then), else JMP to else
             */
            k = (cond_type == AST_BINARY_NE) ? 1 : 0;

            // Emit OP_ISNULL instruction and jump
            xemit_isnull(compiler->emitter, rb, k);
            int else_jump = emit_jump(compiler->emitter, OP_JMP);

            // Compile then branch
            xr_compile_statement(ctx, compiler, node->then_branch);

            // If has else, skip else branch
            int end_jump = -1;
            if (node->else_branch != NULL) {
                end_jump = emit_jump(compiler->emitter, OP_JMP);
            }

            // Patch else jump
            patch_jump(compiler->emitter, else_jump, -1);

            // Compile else branch
            if (node->else_branch != NULL) {
                xr_compile_statement(ctx, compiler, node->else_branch);
                patch_jump(compiler->emitter, end_jump, -1);
            }

            return;  // Return directly, skip subsequent processing
        }

        if (!use_immediate) {
            // Generic path: compile both operands
            XrExprDesc left_expr = xr_compile_expr(ctx, compiler, cmp->left);
            rb = xexpr_to_anyreg_readonly(ctx, compiler, &left_expr);
            XrExprDesc right_expr = xr_compile_expr(ctx, compiler, cmp->right);
            rc = xexpr_to_anyreg_readonly(ctx, compiler, &right_expr);

            rb = xexpr_ensure_boxed(ctx, compiler, &left_expr, rb);
            rc = xexpr_ensure_boxed(ctx, compiler, &right_expr, rc);
            switch (cond_type) {
                case AST_BINARY_EQ: op = OP_EQ; break;
                case AST_BINARY_NE: op = OP_EQ; k = 1; break;
                case AST_BINARY_LT: op = OP_LT; break;
                case AST_BINARY_LE: op = OP_LE; break;
                case AST_BINARY_GT: op = OP_LT; { int tmp = rb; rb = rc; rc = tmp; } break;
                case AST_BINARY_GE: op = OP_LE; { int tmp = rb; rb = rc; rc = tmp; } break;
                default: op = OP_LT; break;
            }
        }

        // Emit comparison instruction
        if (use_immediate) {
            uint8_t c_val = (uint8_t)(imm_value & 0xFF);
            emit_abc(compiler->emitter, op, rb, c_val, k);
        } else {
            emit_abc(compiler->emitter, op, rb, rc, k);
        }

        int else_jump = emit_jump(compiler->emitter, OP_JMP);

        reg_free(compiler, rb);
        if (rc >= 0) {
            reg_free(compiler, rc);
        }

        // Compile then branch
        xr_compile_statement(ctx, compiler, node->then_branch);

        // If has else, skip else branch
        int end_jump = -1;
        if (node->else_branch != NULL) {
            end_jump = emit_jump(compiler->emitter, OP_JMP);
        }

        // Patch else jump
        patch_jump(compiler->emitter, else_jump, -1);

        // Compile else branch
        if (node->else_branch != NULL) {
            xr_compile_statement(ctx, compiler, node->else_branch);
            patch_jump(compiler->emitter, end_jump, -1);
        }
    } else {
        // Normal expression: compile to boolean then test
        XrExprDesc cond_expr = xr_compile_expr(ctx, compiler, node->condition);
        int cond_reg = xexpr_to_anyreg(ctx, compiler, &cond_expr);

        xemit_test(compiler->emitter, cond_reg, 0);
        int then_jump = emit_jump(compiler->emitter, OP_JMP);
        reg_free(compiler, cond_reg);

        // Compile then branch
        xr_compile_statement(ctx, compiler, node->then_branch);

        // Skip else branch
        int else_jump = emit_jump(compiler->emitter, OP_JMP);

        // Patch then_jump
        patch_jump(compiler->emitter, then_jump, -1);

        // Compile else branch (if any)
        if (node->else_branch != NULL) {
            xr_compile_statement(ctx, compiler, node->else_branch);
        }

        // Patch else_jump
        patch_jump(compiler->emitter, else_jump, -1);
    }
}
