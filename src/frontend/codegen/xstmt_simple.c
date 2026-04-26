/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xstmt_simple.c - Simple statement compilation
 *
 * KEY CONCEPT:
 *   Handles simple statement types: expression statements, print,
 *   assignments, compound assignments, increment/decrement.
 *
 *   var-decl + try_extract_comptime path lives in xstmt_typed.c, and
 *   the shared xstmt_emit_* helpers live in xstmt_helpers.{h,c}. This
 *   file stays focused on the "simple" (low-type-machinery) statement
 *   shapes.
 */

#include "xstmt.h"
#include "xstmt_helpers.h"
#include "../../base/xchecks.h"
#include "../../runtime/xisolate_api.h"
#include "../../runtime/value/xtype.h"
#include "xcompiler.h"
#include "xcompiler_context.h"
#include "xemit.h"
#include "xexpr_desc.h"
#include "xexpr.h"
#include "../../runtime/symbol/xsymbol_table.h"
#include "../../runtime/value/xtype_names.h"
#include <stdio.h>
#include <string.h>


/* ========== Expression Statement ========== */

/*
 * Compile expression statement.
 *
 * When expression is used as statement, result is discarded.
 *
 * Special handling: assignment, member set, index set.
 */
void compile_expr_stmt(XrCompilerContext *ctx, XrCompiler *compiler, AstNode *expr) {
    XR_DCHECK(ctx != NULL, "compile_expr_stmt: NULL ctx");
    XR_DCHECK(compiler != NULL, "compile_expr_stmt: NULL compiler");
    XR_DCHECK(expr != NULL, "compile_expr_stmt: NULL expr");
    // Handle assignment statements (they are not expressions)
    if (expr->type == AST_ASSIGNMENT) {
        compile_assignment(ctx, compiler, &expr->as.assignment);
        return;
    }

    // Handle compound assignment statements (they are not expressions)
    if (expr->type == AST_COMPOUND_ASSIGNMENT) {
        compile_compound_assignment(ctx, compiler, &expr->as.compound_assignment);
        return;
    }

    // Handle increment statement
    if (expr->type == AST_INC) {
        compile_inc(ctx, compiler, &expr->as.inc);
        return;
    }

    // Handle decrement statement
    if (expr->type == AST_DEC) {
        compile_dec(ctx, compiler, &expr->as.dec);
        return;
    }

    // Handle member set statement
    if (expr->type == AST_MEMBER_SET) {
        compile_member_set(ctx, compiler, &expr->as.member_set);
        return;
    }

    // Handle index set statement
    if (expr->type == AST_INDEX_SET) {
        compile_index_set(ctx, compiler, &expr->as.index_set);
        return;
    }

    // Optimization: await as statement (no return value) → set C=1 to skip deep copy
    if (expr->type == AST_AWAIT_EXPR) {
        AwaitExprNode *await_node = &expr->as.await_expr;
        XrExprDesc expr_desc = xr_compile_expr(ctx, compiler, await_node->expr);
        int coro_reg = xexpr_to_anyreg(ctx, compiler, &expr_desc);
        int target = reg_alloc(ctx, compiler);
        if (await_node->is_any_success) {
            emit_abc(compiler->emitter, OP_AWAIT_ANY, target, coro_reg, 1);
        } else if (await_node->is_any) {
            emit_abc(compiler->emitter, OP_AWAIT_ANY, target, coro_reg, 0);
        } else if (await_node->is_all || await_node->expr->type == AST_ARRAY_LITERAL) {
            emit_abc(compiler->emitter, OP_AWAIT_ALL, target, coro_reg, 0);
        } else if (await_node->timeout != NULL) {
            XrExprDesc timeout_desc = xr_compile_expr(ctx, compiler, await_node->timeout);
            int timeout_reg = xexpr_to_anyreg(ctx, compiler, &timeout_desc);
            emit_abc(compiler->emitter, OP_AWAIT_TIMEOUT, target, coro_reg, timeout_reg);
        } else {
            // C=1: signal VM to skip deep copy (result discarded)
            emit_abc(compiler->emitter, OP_AWAIT, target, coro_reg, 1);
        }
        reg_free(compiler, target);
        return;
    }

    // Optimization: go as statement (fire-and-forget) → C bit 7 flag in SPAWN_CONT
    // Enables deferred coroutine recycling: child coro won't be awaited.
    if (expr->type == AST_GO_EXPR) {
        int target = reg_alloc(ctx, compiler);
        compile_go_expr(ctx, compiler, &expr->as.go_expr, target, true);
        reg_free(compiler, target);
        return;
    }

    // Other expression statements
    XrExprDesc e = xr_compile_expr(ctx, compiler, expr);

    // Fix: VOID expressions (like push with no return value) don't need register allocation
    if (e.kind == XEXPR_VOID) {
        return;
    }

    int reg = xexpr_to_anyreg(ctx, compiler, &e);
    reg_free(compiler, reg);
}


/* ========== Print Statement ========== */

/*
 * Compile print statement (supports multiple arguments).
 *
 * print(a, b, c) compiles to:
 *   OP_PRINT R[a] 0 0    // first arg, no space, no newline
 *   OP_PRINT R[b] 1 0    // second arg, space before, no newline
 *   OP_PRINT R[c] 1 1    // last arg, space before, newline
 *
 * Instruction format: OP_PRINT A B C
 *   A: value register
 *   B: 1=add space before
 *   C: 1=newline after print
 */
void compile_print(XrCompilerContext *ctx, XrCompiler *compiler, PrintNode *node) {
    XR_DCHECK(ctx != NULL, "compile_print: NULL ctx");
    XR_DCHECK(compiler != NULL, "compile_print: NULL compiler");
    XR_DCHECK(node != NULL, "compile_print: NULL node");
    // If no arguments, just print newline
    if (node->expr_count == 0) {
        // Print empty string to trigger newline
        int reg = reg_alloc(ctx, compiler);
        XrString *empty = xr_compile_time_intern(ctx->X, "", 0);
        int const_idx = xr_vm_proto_add_constant(compiler->proto, xr_string_value(empty));
        emit_abx(compiler->emitter, OP_LOADK, reg, const_idx);
        emit_abc(compiler->emitter, OP_PRINT, reg, 0, 1);  // C=1 newline
        reg_free(compiler, reg);
        return;
    }

    // Compile and print each expression
    for (int i = 0; i < node->expr_count; i++) {
        XrExprDesc expr = xr_compile_expr(ctx, compiler, node->exprs[i]);

        // C: bit0=newline, bit1-2=slot_hint (0=ANY, 1=I64, 2=F64)
        int slot_hint = 0;
        if (xexpr_is_raw_i64(&expr))      slot_hint = 1;
        else if (xexpr_is_raw_f64(&expr)) slot_hint = 2;

        // Typed values: readonly (no BOX), hint tells VM the raw type
        // Any values: anyreg (auto-BOX if needed)
        int reg = (slot_hint != 0)
            ? xexpr_to_anyreg_readonly(ctx, compiler, &expr)
            : xexpr_to_anyreg(ctx, compiler, &expr);

        int add_space = (i > 0) ? 1 : 0;
        int newline = (i == node->expr_count - 1) ? 1 : 0;
        int c_field = newline | (slot_hint << 1);

        emit_abc(compiler->emitter, OP_PRINT, reg, add_space, c_field);
        reg_free(compiler, reg);
    }
}

/* ========== Assignment Statement ========== */

/*
 * Compile assignment statement.
 *
 * Handles local variables, upvalue, and global variable assignment.
 */
void compile_assignment(XrCompilerContext *ctx, XrCompiler *compiler, AssignmentNode *node) {
    XR_DCHECK(ctx != NULL, "compile_assignment: NULL ctx");
    XR_DCHECK(compiler != NULL, "compile_assignment: NULL compiler");
    XR_DCHECK(node != NULL, "compile_assignment: NULL node");
    // Create string
    XrString *name_str = xr_compile_time_intern(ctx->X, node->name, strlen(node->name));

    // Check if trying to overwrite constant (including fn-declared functions and const variables)
    XrLocalInfo *local_info = compiler_get_local_by_name(compiler, node->name);
    if (local_info && local_info->is_const) {
        xr_compiler_error(ctx, compiler, "Cannot modify constant '%s'", node->name);
        return;
    }

    // Check shared constant (considering lexical scope)
    // Skip if local variable exists: local shadows shared
    int shared_index = shared_get_in_scope(ctx, compiler, name_str);
    if (!local_info && shared_index >= 0 && shared_is_const(ctx, shared_index)) {
        xr_compiler_error(ctx, compiler, "Cannot modify shared const '%s'", node->name);
        return;
    }

    // Builtins are read-only, cannot be assigned
    if (!local_info && shared_index < 0 && builtin_get(ctx, name_str) >= 0) {
        xr_compiler_error(ctx, compiler, "Cannot assign to built-in '%s'", node->name);
        return;
    }

    // First lookup variable, then decide how to compile expression
    int local = scope_resolve_local(compiler, name_str);
    if (local >= 0) {
        // Unified type compatibility check (first gate: compile-time)
        // Covers all typed locals: int, float, string, bool, Array, Map, class, etc.
        // Skip: unknown (unresolved), null (inferred from null literal = untyped)
        if (local_info && local_info->compile_type) {
            XrType *target_type = local_info->compile_type;
            if (target_type->kind != XR_KIND_UNKNOWN) {
                XrType *expr_type = get_expr_type(ctx, compiler, node->value);
                // Strip nullable from source: int? → int for base type check
                // (runtime UNBOX handles actual null values)
                XrType *check_source = expr_type;
                if (check_source && check_source->is_nullable) {
                    XrType *base = xr_type_non_nullable(ctx->X, check_source);
                    if (base) check_source = base;
                }
                if (check_source && !xr_type_assignable(target_type, check_source)) {
                    // Json/JsonValue→primitive/union: allowed with runtime type check (OP_CHECKTYPE)
                    if (!xr_is_json_coercion(target_type, check_source)) {
                        if (XR_TYPE_IS_INT(target_type) && XR_TYPE_IS_FLOAT(check_source)) {
                            xr_compiler_error(ctx, compiler,
                                "Cannot assign float to int variable '%s' (use int() for explicit conversion)",
                                node->name);
                        } else {
                            xr_compiler_error(ctx, compiler,
                                "Cannot assign %s to %s variable '%s'",
                                xstmt_type_flag_name(check_source), xstmt_type_flag_name(target_type), node->name);
                        }
                        return;
                    }
                }
            }
        }

        if (local_info && local_info->is_cellified) {
            // Cellified local: write through existing cell (CELL_SET).
            // Must NOT overwrite the cell ref in R[local].
            XrExprDesc expr = xr_compile_expr(ctx, compiler, node->value);
            int value_reg = xexpr_to_anyreg(ctx, compiler, &expr);
            if (local_info) xstmt_emit_box_if_raw(compiler->emitter, value_reg, &expr);
            if (local_info && local_info->compile_type)
                xstmt_emit_json_checktype(ctx, compiler, value_reg, local_info->compile_type, node->value);
            emit_abc(compiler->emitter, OP_CELL_SET, local, value_reg, 0);
            reg_free(compiler, value_reg);
        } else {
            // Normal local: discharge directly to target register
            XrExprDesc expr = xr_compile_expr(ctx, compiler, node->value);
            xexpr_to_specific_reg(ctx, compiler, &expr, local);
            // BOX if assigning raw to tagged local
            if (local_info) {
                xstmt_emit_box_if_raw(compiler->emitter, local, &expr);
            }
            if (local_info && local_info->compile_type)
                xstmt_emit_json_checktype(ctx, compiler, local, local_info->compile_type, node->value);
            // Struct value semantics: copy-on-assign
            if (local_info && local_info->compile_type && local_info->compile_type->is_value_type) {
                AstNodeType val_type = node->value->type;
                if (val_type != AST_STRUCT_LITERAL && val_type != AST_NEW_EXPR) {
                    xstmt_emit_value_copy(ctx, compiler, local, local, local_info->compile_type);
                }
            }
        }
    } else if (shared_index >= 0) {
        // shared variable assignment (xexpr_to_anyreg auto-BOXes typed values)
        XrExprDesc expr = xr_compile_expr(ctx, compiler, node->value);
        int value_reg = xexpr_to_anyreg(ctx, compiler, &expr);
        emit_abx(compiler->emitter, OP_SETSHARED, value_reg, shared_index);
        reg_free(compiler, value_reg);
    } else {
        // Non-local variable: normal compilation (xexpr_to_anyreg auto-BOXes typed values)
        XrExprDesc expr = xr_compile_expr(ctx, compiler, node->value);
        int value_reg = xexpr_to_anyreg(ctx, compiler, &expr);

        int upvalue = scope_resolve_upvalue(ctx, compiler, name_str);
        if (upvalue >= 0) {
            // Upvalue assignment: UPVAL_GET (cell ref) + CELL_SET
            int cell_reg = reg_alloc(ctx, compiler);
            emit_abc(compiler->emitter, OP_UPVAL_GET, cell_reg, upvalue, 0);
            emit_abc(compiler->emitter, OP_CELL_SET, cell_reg, value_reg, 0);
            reg_free(compiler, cell_reg);
            reg_free(compiler, value_reg);
        } else {
            // Top-level variable assignment - check shared first, then predefined globals
            int shared_index = shared_get_in_scope(ctx, compiler, name_str);
            if (shared_index >= 0) {
                emit_abx(compiler->emitter, OP_SETSHARED, value_reg, shared_index);
                reg_free(compiler, value_reg);
            } else {
                int global_index = builtin_get(ctx, name_str);
                if (global_index < 0) {
                    xr_compiler_error(ctx, compiler,
                        "Undefined variable '%s', use 'let %s = ...' to define first",
                        name_str->data, name_str->data);
                    reg_free(compiler, value_reg);
                    return;
                }
                xr_compiler_error(ctx, compiler,
                    "Cannot assign to built-in '%s'", name_str->data);
                reg_free(compiler, value_reg);
            }
        }
    }
}

// Destructuring compilation is in xstmt_destructure.c

/*
 * Compile compound assignment (e.g., +=, -=, *=, /=, %=, &=, |=, ^=, <<=, >>=)
 *
 * Decomposed to individual binary ops: R[A] = R[A] op R[B].
 * No fused OP_COMPOUND_ASSIGN — each operator maps to a standard opcode
 * (OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD, OP_BAND, OP_BOR, OP_BXOR,
 * OP_SHL, OP_SHR), giving JIT/AOT independent type-inference per op.
 */
void compile_compound_assignment(XrCompilerContext *ctx, XrCompiler *compiler, CompoundAssignmentNode *node) {
    XR_DCHECK(ctx != NULL, "compile_compound_assignment: NULL ctx");
    XR_DCHECK(compiler != NULL, "compile_compound_assignment: NULL compiler");
    XR_DCHECK(node != NULL, "compile_compound_assignment: NULL node");

    // Map token to binary opcode
    OpCode binary_op;
    switch (node->op) {
        case TK_PLUS_ASSIGN:   binary_op = OP_ADD;  break;
        case TK_MINUS_ASSIGN:  binary_op = OP_SUB;  break;
        case TK_MUL_ASSIGN:    binary_op = OP_MUL;  break;
        case TK_DIV_ASSIGN:    binary_op = OP_DIV;  break;
        case TK_MOD_ASSIGN:    binary_op = OP_MOD;  break;
        case TK_AND_ASSIGN:    binary_op = OP_BAND; break;
        case TK_OR_ASSIGN:     binary_op = OP_BOR;  break;
        case TK_XOR_ASSIGN:    binary_op = OP_BXOR; break;
        case TK_LSHIFT_ASSIGN: binary_op = OP_SHL;  break;
        case TK_RSHIFT_ASSIGN: binary_op = OP_SHR;  break;
        default:
            xr_compiler_error(ctx, compiler, "Unsupported compound assignment operator");
            return;
    }

    bool is_arithmetic = (binary_op == OP_ADD || binary_op == OP_SUB ||
                          binary_op == OP_MUL || binary_op == OP_DIV ||
                          binary_op == OP_MOD);

    // Check if member access compound assignment (this.field += value)
    if (node->object != NULL) {
        /* === Member access compound assignment ===
         * Strategy:
         * 1. Compile object to register
         * 2. Read member value to temp register
         * 3. Execute binary operation: R[member] = R[member] op R[value]
         * 4. Write back member value
         */

        // Compile object expression
        XrExprDesc obj_expr = xr_compile_expr(ctx, compiler, node->object);
        int obj_reg = xexpr_to_anyreg(ctx, compiler, &obj_expr);

        // Member name symbol index
        int global_sym = xr_symbol_register_in_table(
            (XrSymbolTable*)xr_isolate_get_symbol_table(ctx->X), node->name);
        int local_sym = emitter_add_symbol(compiler->emitter, global_sym);

        // Allocate temp register to store member value
        int member_reg = reg_alloc(ctx, compiler);

        // Read current member value: R[member_reg] = R[obj_reg].prop
        emit_abc(compiler->emitter, OP_GETPROP, member_reg, obj_reg, local_sym);

        // Compile right-side expression
        XrExprDesc value_expr = xr_compile_expr(ctx, compiler, node->value);
        int value_reg = xexpr_to_anyreg(ctx, compiler, &value_expr);

        // R[member_reg] = R[member_reg] op R[value_reg]
        emit_abc(compiler->emitter, binary_op, member_reg, member_reg, value_reg);

        // Write back member value: R[obj_reg].prop = R[member_reg]
        emit_abc(compiler->emitter, OP_SETPROP, obj_reg, local_sym, member_reg);

        // Free registers
        reg_free(compiler, value_reg);
        reg_free(compiler, member_reg);
        reg_free(compiler, obj_reg);
        return;
    }

    /* === Normal variable compound assignment === */
    // Create string
    XrString *name_str = xr_compile_time_intern(ctx->X, node->name, strlen(node->name));

    // Lookup variable and determine type
    int local = scope_resolve_local(compiler, name_str);
    int upvalue = -1;
    int shared_index = -1;
    int global_index = -1;
    int var_reg = -1;
    bool need_writeback = false;
    XrLocalInfo *ca_info = (local >= 0) ? compiler_get_local_by_name(compiler, node->name) : NULL;

    if (local >= 0) {
        // Type compatibility check for compound assignment
        if (ca_info && ca_info->compile_type && XR_TYPE_IS_INT(ca_info->compile_type) && is_arithmetic) {
            XrType *rhs_type = get_expr_type(ctx, compiler, node->value);
            if (rhs_type && XR_TYPE_IS_FLOAT(rhs_type) && !XR_TYPE_IS_INT(rhs_type)) {
                xr_compiler_error(ctx, compiler,
                    "Cannot use float in compound assignment to int variable '%s' (use int() for explicit conversion)",
                    node->name);
                return;
            }
            if (rhs_type && !XR_TYPE_IS_INT(rhs_type) && !XR_TYPE_IS_FLOAT(rhs_type)) {
                xr_compiler_error(ctx, compiler,
                    "Cannot use non-numeric value in compound assignment to int variable '%s'",
                    node->name);
                return;
            }
        }
        if (ca_info && ca_info->is_cellified) {
            // Cellified local: load current value from cell into temp
            var_reg = reg_alloc(ctx, compiler);
            emit_abc(compiler->emitter, OP_CELL_GET, var_reg, local, 0);
            need_writeback = true;
        } else {
            // Normal local: use its register directly
            var_reg = local;
        }
    } else {
        upvalue = scope_resolve_upvalue(ctx, compiler, name_str);
        if (upvalue >= 0) {
            var_reg = reg_alloc(ctx, compiler);
            // Upvalue: load cell ref, then deref cell
            int cell_reg = reg_alloc(ctx, compiler);
            emit_abc(compiler->emitter, OP_UPVAL_GET, cell_reg, upvalue, 0);
            emit_abc(compiler->emitter, OP_CELL_GET, var_reg, cell_reg, 0);
            reg_free(compiler, cell_reg);
            need_writeback = true;
        } else {
            shared_index = shared_get_in_scope(ctx, compiler, name_str);
            if (shared_index >= 0) {
                var_reg = reg_alloc(ctx, compiler);
                emit_abx(compiler->emitter, OP_GETSHARED, var_reg, shared_index);
                need_writeback = true;
            } else {
                global_index = builtin_get(ctx, name_str);
                if (global_index >= 0) {
                    var_reg = reg_alloc(ctx, compiler);
                    emit_abx(compiler->emitter, OP_GETBUILTIN, var_reg, global_index);
                    need_writeback = true;
                } else {
                    xr_compiler_error(ctx, compiler, "Undefined variable '%s'", name_str->data);
                    return;
                }
            }
        }
    }

    // Compile right-side expression
    XrExprDesc value_expr = xr_compile_expr(ctx, compiler, node->value);
    int value_reg = xexpr_to_anyreg(ctx, compiler, &value_expr);

    value_reg = xexpr_ensure_boxed(ctx, compiler, &value_expr, value_reg);
    // R[var_reg] = R[var_reg] op R[value_reg]
    emit_abc(compiler->emitter, binary_op, var_reg, var_reg, value_reg);

    // If needed, write back variable
    if (need_writeback) {
        if (local >= 0 && ca_info && ca_info->is_cellified) {
            // Cellified local: write back through cell
            emit_abc(compiler->emitter, OP_CELL_SET, local, var_reg, 0);
        } else if (upvalue >= 0) {
            // Upvalue: write back through cell
            int cell_reg = reg_alloc(ctx, compiler);
            emit_abc(compiler->emitter, OP_UPVAL_GET, cell_reg, upvalue, 0);
            emit_abc(compiler->emitter, OP_CELL_SET, cell_reg, var_reg, 0);
            reg_free(compiler, cell_reg);
        } else if (shared_index >= 0) {
            emit_abx(compiler->emitter, OP_SETSHARED, var_reg, shared_index);
        } else {
            xr_compiler_error(ctx, compiler, "Cannot assign to built-in '%s'", name_str->data);
        }
        reg_free(compiler, var_reg);
    }

    reg_free(compiler, value_reg);
}


/*
 * Compile increment statement.
 * ++x or x++ (uniformly handled as prefix)
 * Decomposed to ADDI R[A] R[A] 1 (no fused OP_INC).
 */
void compile_inc(XrCompilerContext *ctx, XrCompiler *compiler, IncDecNode *node) {
    XR_DCHECK(ctx != NULL, "compile_inc: NULL ctx");
    XR_DCHECK(compiler != NULL, "compile_inc: NULL compiler");
    XR_DCHECK(node != NULL, "compile_inc: NULL node");
    XrString *name_str = xr_compile_time_intern(ctx->X, node->name, strlen(node->name));

    int local = scope_resolve_local(compiler, name_str);
    int upvalue = -1;
    int shared_index = -1;
    int global_index = -1;
    int var_reg = -1;
    bool need_writeback = false;

    XrLocalInfo *local_info = (local >= 0) ? compiler_get_local_by_name(compiler, node->name) : NULL;
    if (local >= 0) {
        if (local_info && local_info->is_cellified) {
            var_reg = reg_alloc(ctx, compiler);
            emit_abc(compiler->emitter, OP_CELL_GET, var_reg, local, 0);
            need_writeback = true;
        } else {
            var_reg = local;
        }
    } else {
        upvalue = scope_resolve_upvalue(ctx, compiler, name_str);
        if (upvalue >= 0) {
            var_reg = reg_alloc(ctx, compiler);
            int cell_reg = reg_alloc(ctx, compiler);
            emit_abc(compiler->emitter, OP_UPVAL_GET, cell_reg, upvalue, 0);
            emit_abc(compiler->emitter, OP_CELL_GET, var_reg, cell_reg, 0);
            reg_free(compiler, cell_reg);
            need_writeback = true;
        } else {
            shared_index = shared_get_in_scope(ctx, compiler, name_str);
            if (shared_index >= 0) {
                var_reg = reg_alloc(ctx, compiler);
                emit_abx(compiler->emitter, OP_GETSHARED, var_reg, shared_index);
                need_writeback = true;
            } else {
                global_index = builtin_get(ctx, name_str);
                if (global_index >= 0) {
                    var_reg = reg_alloc(ctx, compiler);
                    emit_abx(compiler->emitter, OP_GETBUILTIN, var_reg, global_index);
                    need_writeback = true;
                } else {
                    xr_compiler_error(ctx, compiler, "Undefined variable '%s'", name_str->data);
                    return;
                }
            }
        }
    }

    emit_abc(compiler->emitter, OP_ADDI, var_reg, var_reg, 1);

    if (need_writeback) {
        if (local >= 0 && local_info && local_info->is_cellified) {
            emit_abc(compiler->emitter, OP_CELL_SET, local, var_reg, 0);
        } else if (upvalue >= 0) {
            int cell_reg2 = reg_alloc(ctx, compiler);
            emit_abc(compiler->emitter, OP_UPVAL_GET, cell_reg2, upvalue, 0);
            emit_abc(compiler->emitter, OP_CELL_SET, cell_reg2, var_reg, 0);
            reg_free(compiler, cell_reg2);
        } else if (shared_index >= 0) {
            emit_abx(compiler->emitter, OP_SETSHARED, var_reg, shared_index);
        } else {
            xr_compiler_error(ctx, compiler, "Cannot assign to built-in '%s'", name_str->data);
        }
        reg_free(compiler, var_reg);
    }
}

/*
 * Compile decrement statement.
 * --x or x-- (uniformly handled as prefix)
 * Decomposed to SUBI R[A] R[A] 1 (no fused OP_DEC).
 */
void compile_dec(XrCompilerContext *ctx, XrCompiler *compiler, IncDecNode *node) {
    XR_DCHECK(ctx != NULL, "compile_dec: NULL ctx");
    XR_DCHECK(compiler != NULL, "compile_dec: NULL compiler");
    XR_DCHECK(node != NULL, "compile_dec: NULL node");
    XrString *name_str = xr_compile_time_intern(ctx->X, node->name, strlen(node->name));

    int local = scope_resolve_local(compiler, name_str);
    int upvalue = -1;
    int shared_index = -1;
    int global_index = -1;
    int var_reg = -1;
    bool need_writeback = false;

    XrLocalInfo *local_info_dec = (local >= 0) ? compiler_get_local_by_name(compiler, node->name) : NULL;
    if (local >= 0) {
        if (local_info_dec && local_info_dec->is_cellified) {
            var_reg = reg_alloc(ctx, compiler);
            emit_abc(compiler->emitter, OP_CELL_GET, var_reg, local, 0);
            need_writeback = true;
        } else {
            var_reg = local;
        }
    } else {
        upvalue = scope_resolve_upvalue(ctx, compiler, name_str);
        if (upvalue >= 0) {
            var_reg = reg_alloc(ctx, compiler);
            int cell_reg = reg_alloc(ctx, compiler);
            emit_abc(compiler->emitter, OP_UPVAL_GET, cell_reg, upvalue, 0);
            emit_abc(compiler->emitter, OP_CELL_GET, var_reg, cell_reg, 0);
            reg_free(compiler, cell_reg);
            need_writeback = true;
        } else {
            shared_index = shared_get_in_scope(ctx, compiler, name_str);
            if (shared_index >= 0) {
                var_reg = reg_alloc(ctx, compiler);
                emit_abx(compiler->emitter, OP_GETSHARED, var_reg, shared_index);
                need_writeback = true;
            } else {
                global_index = builtin_get(ctx, name_str);
                if (global_index >= 0) {
                    var_reg = reg_alloc(ctx, compiler);
                    emit_abx(compiler->emitter, OP_GETBUILTIN, var_reg, global_index);
                    need_writeback = true;
                } else {
                    xr_compiler_error(ctx, compiler, "Undefined variable '%s'", name_str->data);
                    return;
                }
            }
        }
    }

    emit_abc(compiler->emitter, OP_SUBI, var_reg, var_reg, 1);

    if (need_writeback) {
        if (local >= 0 && local_info_dec && local_info_dec->is_cellified) {
            emit_abc(compiler->emitter, OP_CELL_SET, local, var_reg, 0);
        } else if (upvalue >= 0) {
            int cell_reg = reg_alloc(ctx, compiler);
            emit_abc(compiler->emitter, OP_UPVAL_GET, cell_reg, upvalue, 0);
            emit_abc(compiler->emitter, OP_CELL_SET, cell_reg, var_reg, 0);
            reg_free(compiler, cell_reg);
        } else if (shared_index >= 0) {
            emit_abx(compiler->emitter, OP_SETSHARED, var_reg, shared_index);
        } else {
            xr_compiler_error(ctx, compiler, "Cannot assign to built-in '%s'", name_str->data);
        }
        reg_free(compiler, var_reg);
    }
}
