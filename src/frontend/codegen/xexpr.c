/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xexpr.c - Xray expression compiler main entry
 *
 * KEY CONCEPT:
 *   - Expression type dispatch
 *   - Coordinate sub-modules
 */

#include "xexpr.h"
#include "../../base/xchecks.h"
#include "xcompiler.h"
#include "xcompiler_context.h"
#include "xemit.h"
#include "xregalloc.h"
#include "xstmt.h"
#include <stdio.h>
#include <string.h>

// Forward declarations
XrExprDesc compile_ternary(XrCompilerContext *ctx, XrCompiler *compiler, TernaryNode *node);
XrExprDesc compile_nullish_coalesce(XrCompilerContext *ctx, XrCompiler *compiler, BinaryNode *node);
XrExprDesc compile_optional_chain(XrCompilerContext *ctx, XrCompiler *compiler, OptionalChainNode *node);
int compile_force_unwrap(XrCompilerContext *ctx, XrCompiler *compiler, UnaryNode *node);
int compile_as_expr(XrCompilerContext *ctx, XrCompiler *compiler, AsExprNode *node);
int compile_range(XrCompilerContext *ctx, XrCompiler *compiler, RangeNode *node);

// ========== Range Expression ==========

/*
 * Compile range expression (start..end)
 *
 * Creates a lazy Range object via OP_NEWRANGE.
 * The Range holds start/end/step without materializing elements.
 * for-in loops on Range are optimized separately in xstmt_forin.c.
 *
 * @return result register number
 */
int compile_range(XrCompilerContext *ctx, XrCompiler *compiler, RangeNode *node) {
    if (!node || !node->start || !node->end) {
        xr_compiler_error(ctx, compiler, "Invalid range expression");
        return reg_alloc(ctx, compiler);
    }

    // Compile start and end values
    XrExprDesc start_expr = xr_compile_expr(ctx, compiler, node->start);
    int start_reg = xexpr_to_anyreg(ctx, compiler, &start_expr);
    XrExprDesc end_expr = xr_compile_expr(ctx, compiler, node->end);
    int end_reg = xexpr_to_anyreg(ctx, compiler, &end_expr);

    // Allocate result register
    int result_reg = reg_alloc(ctx, compiler);

    // OP_NEWRANGE: R[result] = Range(R[start], R[end])
    xemit_newrange(compiler->emitter, result_reg, start_reg, end_reg);

    // Reclaim temporary registers
    xreg_set_freereg(compiler->regalloc, result_reg + 1);

    return result_reg;
}

// ========== Main Expression Compilation Entry ==========

/*
 * Expression compilation entry returning XrExprDesc
 *
 * Supports deferred register allocation and instruction backpatching
 */
XrExprDesc xr_compile_expr(XrCompilerContext *ctx, XrCompiler *c, AstNode *node) {
    XR_DCHECK(ctx != NULL, "compile_expr: NULL ctx");
    XR_DCHECK(c != NULL, "compile_expr: NULL compiler");
    XrExprDesc e;
    xexpr_init(&e, XEXPR_VOID, -1);

    if (!node) {
        return e;
    }

    switch (node->type) {
        // === Literals ===
        case AST_LITERAL_INT:
        case AST_LITERAL_FLOAT:
        case AST_LITERAL_BIGINT:
        case AST_LITERAL_STRING:
        case AST_LITERAL_REGEX:
        case AST_LITERAL_NULL:
        case AST_LITERAL_TRUE:
        case AST_LITERAL_FALSE:
            e = compile_literal(ctx, c, (LiteralNode*)&node->as);
            break;

        // === Binary Operations ===
        case AST_BINARY_ADD:
        case AST_BINARY_SUB:
        case AST_BINARY_MUL:
        case AST_BINARY_DIV:
        case AST_BINARY_MOD:
        case AST_BINARY_BAND:
        case AST_BINARY_BOR:
        case AST_BINARY_BXOR:
        case AST_BINARY_LSHIFT:
        case AST_BINARY_RSHIFT:
            e = compile_binary(ctx, c, (BinaryNode*)&node->as, node->type);
            break;

        // === Comparison Operations ===
        case AST_BINARY_EQ:
        case AST_BINARY_NE:
        case AST_BINARY_EQ_STRICT:
        case AST_BINARY_NE_STRICT:
        case AST_BINARY_LT:
        case AST_BINARY_LE:
        case AST_BINARY_GT:
        case AST_BINARY_GE:
            e = compile_comparison(ctx, c, (BinaryNode*)&node->as, node->type);
            break;

        // === Type Check (is) ===
        case AST_IS_EXPR:
            e = compile_is_expr(ctx, c, (IsExprNode*)&node->as);
            break;

        // === Logical Operations (short-circuit evaluation) ===
        case AST_BINARY_AND: {
            int reg = compile_and(ctx, c, (BinaryNode*)&node->as);
            xexpr_init(&e, XEXPR_TEMP, reg);
            break;
        }
        case AST_BINARY_OR: {
            int reg = compile_or(ctx, c, (BinaryNode*)&node->as);
            xexpr_init(&e, XEXPR_TEMP, reg);
            break;
        }

        // === Ternary Expression ===
        case AST_TERNARY:
            e = compile_ternary(ctx, c, (TernaryNode*)&node->as);
            break;

        // === Nullish Coalescing ===
        case AST_NULLISH_COALESCE:
            e = compile_nullish_coalesce(ctx, c, (BinaryNode*)&node->as);
            break;

        // === Optional Chaining ===
        case AST_OPTIONAL_CHAIN:
            e = compile_optional_chain(ctx, c, (OptionalChainNode*)&node->as);
            break;

        // === Force Unwrap ===
        case AST_FORCE_UNWRAP: {
            int reg = compile_force_unwrap(ctx, c, (UnaryNode*)&node->as);
            xexpr_init(&e, XEXPR_TEMP, reg);
            break;
        }

        // === As Type Cast ===
        case AST_AS_EXPR: {
            int reg = compile_as_expr(ctx, c, (AsExprNode*)&node->as);
            xexpr_init(&e, XEXPR_TEMP, reg);
            break;
        }

        // === Range Expression ===
        case AST_RANGE: {
            int reg = compile_range(ctx, c, (RangeNode*)&node->as);
            xexpr_init(&e, XEXPR_TEMP, reg);
            break;
        }

        // === Unary Operations ===
        case AST_UNARY_NEG:
        case AST_UNARY_NOT:
        case AST_UNARY_BNOT:
            e = compile_unary(ctx, c, (UnaryNode*)&node->as, node->type);
            break;

        // === Variable Access ===
        case AST_VARIABLE:
            e = compile_variable(ctx, c, (VariableNode*)&node->as);
            break;

        // === Function Call ===
        case AST_CALL_EXPR:
            e = compile_call(ctx, c, (CallExprNode*)&node->as);
            break;

        // === Arrays and Collections ===
        case AST_ARRAY_LITERAL:
            e = compile_array_literal(ctx, c, (ArrayLiteralNode*)&node->as);
            break;

        case AST_OBJECT_LITERAL:
            e = compile_object_literal(ctx, c, (ObjectLiteralNode*)&node->as);
            break;

        case AST_MAP_LITERAL:
            e = compile_map_literal(ctx, c, (MapLiteralNode*)&node->as);
            break;

        case AST_SET_LITERAL:
            e = compile_set_literal(ctx, c, (SetLiteralNode*)&node->as);
            break;

        case AST_INDEX_GET:
            e = compile_index_get(ctx, c, (IndexGetNode*)&node->as);
            break;

        case AST_SLICE_EXPR:
            e = compile_slice_expr(ctx, c, (SliceExprNode*)&node->as);
            break;

        // === Objects and Classes ===
        case AST_NEW_EXPR:
            e = compile_new_expr(ctx, c, (NewExprNode*)&node->as);
            break;

        case AST_STRUCT_LITERAL:
            e = compile_struct_literal(ctx, c, &node->as.struct_literal);
            break;

        case AST_MEMBER_ACCESS:
            e = compile_member_access(ctx, c, (MemberAccessNode*)&node->as);
            break;

        // === Enums ===
        case AST_ENUM_ACCESS:
            e = compile_enum_access(ctx, c, (EnumAccessNode*)&node->as);
            break;

        case AST_ENUM_CONVERT:
            e = compile_enum_convert(ctx, c, (EnumConvertNode*)&node->as);
            break;

        case AST_ENUM_INDEX:
            e = compile_enum_index(ctx, c, (EnumIndexNode*)&node->as);
            break;

        // === Match Expression ===
        case AST_MATCH_EXPR: {
            int reg = compile_match_expr(ctx, c, (MatchExprNode*)&node->as);
            xexpr_init(&e, XEXPR_TEMP, reg);
            break;
        }

        // === Class System Expressions ===
        case AST_THIS_EXPR: {
            // this expression: first lookup local variable, then upvalue
            XrString *this_str = xr_compile_time_intern(ctx->X, "this", 4);

            // 1. Lookup local variable (when current function is class method, this is in register 0)
            XrLocalInfo *local_info = compiler_get_local_by_name(c, this_str->data);
            if (local_info) {
                xexpr_init(&e, XEXPR_LOCAL, local_info->reg);
                break;
            }

            // 2. Lookup upvalue (closure captures outer this)
            int upvalue = scope_resolve_upvalue(ctx, c, this_str);
            if (upvalue >= 0) {
                int pc;
                // 'this' is always const (immutable receiver), direct read
                pc = xemit_upval_get(c->emitter, 0, upvalue, 0);
                xexpr_init(&e, XEXPR_RELOC, 0);
                e.u.pc = pc;
                break;
            }

            // 3. this not found, error
            xr_compiler_error(ctx, c, "using 'this' outside of class method");
            xexpr_init(&e, XEXPR_VOID, -1);
            break;
        }

        case AST_SUPER_CALL: {
            // super() or super.method() call
            SuperCallNode *super_call = &node->as.super_call;
            int arg_count = super_call->arg_count;

            // Determine method name: super() uses "constructor", super.method() uses actual method name
            const char *method_name = super_call->method_name != NULL
                                      ? super_call->method_name
                                      : XR_KEYWORD_CONSTRUCTOR;

            /*
             * super.method() call
             * Layout: R[base] = return value, R[base+1] = this, R[base+2..] = args
             * This is consistent with OP_INVOKE layout
             */

            // LIFO mode: directly allocate consecutive registers and protect
            int call_base = xreg_get_freereg(c->regalloc);
            int protect_id = xreg_protect_begin(c->regalloc, call_base, 2, "super_invoke");
            xreg_reserve(c->regalloc, 2);

            // R[base+1] = this (copy from R[0])
            xemit_move(c->emitter, call_base + 1, 0);
            xreg_set_freereg(c->regalloc, call_base + 2);

            /*
             * Compile arguments to R[base+2..] (call_base and call_base+1 already protected)
             * Key: update freereg to target position +1 before compiling each argument,
             * prevent argument expression temp registers from overwriting compiled arguments
             */
            for (int i = 0; i < arg_count; i++) {
                int target_reg = call_base + 2 + i;
                xreg_set_freereg(c->regalloc, target_reg + 1);
                XrExprDesc arg = xr_compile_expr(ctx, c, super_call->arguments[i]);
                xexpr_to_specific_reg(ctx, c, &arg, target_reg);
            }

            // End protection
            xreg_protect_end(c->regalloc, protect_id);

            // Set final freereg
            xreg_set_freereg(c->regalloc, call_base + 1);

            // Method name constant
            XrString *method_str = xr_compile_time_intern(ctx->X, method_name,
                                                    strlen(method_name));
            int method_const = xr_vm_proto_add_constant(c->proto, xr_string_value(method_str));

            // Generate OP_SUPERINVOKE: A=call_base, B=method name constant, C=arg count
            xemit_superinvoke(c->emitter, call_base, method_const, arg_count);

            // Result in R[call_base]
            xexpr_init(&e, XEXPR_TEMP, call_base);
            break;
        }

        // === Function Expression (arrow functions etc) ===
        case AST_FUNCTION_EXPR: {
            FunctionDeclNode *func_expr = &node->as.function_expr;

            XrCompiler function_compiler;
            xr_compiler_init(ctx, &function_compiler, FUNCTION_FUNCTION);
            function_compiler.enclosing = c;
            function_compiler.proto->numparams = func_expr->param_count;

            scope_begin(&function_compiler);

            // Pre-scan before params: mark captured params before codegen
            prescan_fn_body(&function_compiler, func_expr, func_expr->body);

            // Define parameters as local variables (using registers 0, 1, 2...)
            for (int i = 0; i < func_expr->param_count; i++) {
                XrParamNode *param = func_expr->params[i];
                if (!param) continue;
                XrString *param_str = xr_compile_time_intern(ctx->X, param->name, strlen(param->name));
                scope_define_local_reg(ctx, &function_compiler, param_str, i);
            }

            if (function_compiler.regalloc) {
                xreg_set_freereg(function_compiler.regalloc, xreg_get_local_end(function_compiler.regalloc));
            }

            xr_compile_statement(ctx, &function_compiler, func_expr->body);
            XrProto *fn_proto = xr_compiler_end(ctx, &function_compiler);

            if (fn_proto != NULL) {
                int proto_idx = xr_vm_proto_add_proto(c->proto, fn_proto);
                int result_reg = reg_alloc(ctx, c);
                emit_ctx_sync_before_closure(ctx, c);
                xemit_closure(c->emitter, result_reg, proto_idx);
                xexpr_init(&e, XEXPR_TEMP, result_reg);
            } else {
                int result_reg = reg_alloc(ctx, c);
                xexpr_init(&e, XEXPR_TEMP, result_reg);
            }
            break;
        }

        // === Expression Block ===
        case AST_BLOCK: {
            BlockNode *block = &node->as.block;
            scope_begin(c);

            int result_reg = -1;

            for (int i = 0; i < block->count; i++) {
                AstNode *stmt = block->statements[i];

                if (i == block->count - 1) {
                    if (stmt->type == AST_EXPR_STMT) {
                        XrExprDesc last = xr_compile_expr(ctx, c, stmt->as.expr_stmt);
                        result_reg = xexpr_to_anyreg(ctx, c, &last);
                    } else {
                        xr_compile_statement(ctx, c, stmt);
                        result_reg = reg_alloc(ctx, c);
                        xemit_loadnull(c->emitter, result_reg);
                    }
                } else {
                    xr_compile_statement(ctx, c, stmt);
                }
            }

            scope_end(ctx, c);

            int final_reg = reg_alloc(ctx, c);

            if (result_reg == -1) {
                xemit_loadnull(c->emitter, final_reg);
            } else {
                emit_move(c->emitter, final_reg, result_reg);
            }

            xexpr_init(&e, XEXPR_TEMP, final_reg);
            break;
        }

        // === Side-effect Expressions ===
        case AST_ASSIGNMENT: {
            compile_assignment(ctx, c, &node->as.assignment);
            int result_reg = reg_alloc(ctx, c);
            xexpr_init(&e, XEXPR_TEMP, result_reg);
            break;
        }

        case AST_COMPOUND_ASSIGNMENT: {
            compile_compound_assignment(ctx, c, &node->as.compound_assignment);
            int result_reg = reg_alloc(ctx, c);
            xexpr_init(&e, XEXPR_TEMP, result_reg);
            break;
        }

        case AST_DESTRUCTURE_ASSIGN: {
            compile_destructure_assign(ctx, c, &node->as.destructure_assign);
            int result_reg = reg_alloc(ctx, c);
            xexpr_init(&e, XEXPR_TEMP, result_reg);
            break;
        }

        case AST_INC: {
            compile_inc(ctx, c, &node->as.inc);
            int result_reg = reg_alloc(ctx, c);
            xexpr_init(&e, XEXPR_TEMP, result_reg);
            break;
        }

        case AST_DEC: {
            compile_dec(ctx, c, &node->as.dec);
            int result_reg = reg_alloc(ctx, c);
            xexpr_init(&e, XEXPR_TEMP, result_reg);
            break;
        }

        // === Template String ===
        case AST_TEMPLATE_STRING: {
            TemplateStringNode *tmpl = &node->as.template_str;

            // If no parts, return empty string
            if (tmpl->part_count == 0) {
                int result_reg = reg_alloc(ctx, c);
                XrString *empty = xr_compile_time_intern(ctx->X, "", 0);
                int const_idx = xr_vm_proto_add_constant(c->proto, xr_string_value(empty));
                xemit_loadk(c->emitter, result_reg, const_idx);
                xexpr_init(&e, XEXPR_TEMP, result_reg);
                break;
            }

            // If only one part, return directly
            if (tmpl->part_count == 1) {
                e = xr_compile_expr(ctx, c, tmpl->parts[0]);
                break;
            }

            // Use STRBUF sequence for template strings (implicit type conversion)
            int buf_reg = reg_alloc(ctx, c);
            xemit_strbuf_new(c->emitter, buf_reg);

            for (int i = 0; i < tmpl->part_count; i++) {
                XrExprDesc part = xr_compile_expr(ctx, c, tmpl->parts[i]);
                int result = xexpr_to_anyreg(ctx, c, &part);
                xemit_strbuf_append(c->emitter, buf_reg, result);
                if (result != buf_reg) {
                    reg_free(c, result);
                }
            }

            xemit_strbuf_finish(c->emitter, buf_reg);
            xexpr_init(&e, XEXPR_TEMP, buf_reg);
            break;
        }

        // === Other Types ===
        case AST_GROUPING:
            e = xr_compile_expr(ctx, c, node->as.grouping);
            break;

        // === Coroutine Expressions ===
        case AST_GO_EXPR: {
            int result_reg = reg_alloc(ctx, c);
            compile_go_expr(ctx, c, &node->as.go_expr, result_reg, false);
            xexpr_init(&e, XEXPR_TEMP, result_reg);
            break;
        }

        case AST_SCOPE_BLOCK: {
            // supervisor scope as expression: returns errors[]
            int result_reg = reg_alloc(ctx, c);
            compile_scope_block(ctx, c, &node->as.scope_block, result_reg);
            xexpr_init(&e, XEXPR_TEMP, result_reg);
            break;
        }

        case AST_AWAIT_EXPR: {
            int result_reg = reg_alloc(ctx, c);
            compile_await_expr(ctx, c, &node->as.await_expr, result_reg);
            xexpr_init(&e, XEXPR_TEMP, result_reg);
            break;
        }

        case AST_CHANNEL_NEW: {
            int result_reg = reg_alloc(ctx, c);
            compile_channel_new(ctx, c, &node->as.channel_new, result_reg);
            xexpr_init(&e, XEXPR_TEMP, result_reg);
            break;
        }

        case AST_CANCELLED_EXPR: {
            int result_reg = reg_alloc(ctx, c);
            compile_cancelled_expr(ctx, c, result_reg);
            xexpr_init(&e, XEXPR_TEMP, result_reg);
            break;
        }

        case AST_MOVE_EXPR: {
            // move var — compile inner expression; move semantics handled
            // by go/ch.send call sites, not here
            e = xr_compile_expr(ctx, c, node->as.move_expr.expr);
            break;
        }

        default:
            xr_compiler_error(ctx, c, "unknown expression type: %d", node->type);
            return e;
    }

    // Unified fallback: if sub-compiler didn't set compile_type,
    // try to get it from the analyzer's cached type on the AST node.
    if (!e.compile_type && e.kind != XEXPR_VOID) {
        e.compile_type = get_expr_type(ctx, c, node);
    }

    return e;
}

