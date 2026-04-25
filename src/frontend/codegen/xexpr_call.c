/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xexpr_call.c - Function call expression compilation
 *
 * KEY CONCEPT:
 *   Compiles function calls including regular calls, method calls (obj.method()),
 *   recursive calls (CALLSELF optimization), and tail calls (TAILCALL).
 *   Uses expression descriptor system for register allocation.
 */

#include "xexpr.h"
#include "../../base/xchecks.h"
#include "xexpr_desc.h"
#include "xexpr_higher_order.h"
#include "xexpr_call_builtin.h"
#include "xexpr_call_method.h"
#include "xcompiler.h"
#include "xcompiler_context.h"
#include "xcompiler_class_registry.h"
#include "xemit.h"
#include "xregalloc.h"
#include "../parser/xast.h"
#include "../../runtime/value/xtype.h"
#include "../../runtime/value/xtype_names.h"
#include "../../runtime/xisolate_api.h"
#include "../../runtime/symbol/xsymbol_table.h"
#include "../analyzer/xanalyzer_symbol.h"
#include <stdio.h>
#include <string.h>

/* ========== Builtin section moved out (Phase 3, C-02) ==========
 * All `compile_builtin_*` helpers, the `builtin_functions[]` table,
 * and `builtin_lookup` now live in xexpr_call_builtin.{c,h}. Call
 * sites here use xr_compile_call_builtin() as a single dispatcher.
 */


/* ========== Function Call Compilation ========== */

/*
 * Compile function call (internal implementation).
 *
 * Handles:
 * - Method call optimization (obj.method() -> OP_INVOKE)
 * - Recursive call optimization (CALLSELF)
 * - Tail call optimization (TAILCALL)
 * - Regular call (CALL)
 *
 * @param is_tail Whether this is a tail call position
 * @return Result register
 */
int compile_call_internal(XrCompilerContext *ctx, XrCompiler *compiler, CallExprNode *node, bool is_tail, uint8_t *out_slot_type_unused) {
    XR_DCHECK(ctx != NULL, "compile_call_internal: NULL ctx");
    XR_DCHECK(compiler != NULL, "compile_call_internal: NULL compiler");
    XR_DCHECK(node != NULL, "compile_call_internal: NULL node");
    (void)out_slot_type_unused;

    // Builtin function detection via table-driven dispatch.
    // Phase 3 (C-02) lifted the table out to xexpr_call_builtin.c;
    // negative return means "not a builtin / matched-but-bailed", in
    // which case we fall through to the regular dispatch below.
    int builtin_result = xr_compile_call_builtin(ctx, compiler, node, is_tail);
    if (builtin_result >= 0) {
        return builtin_result;
    }

    // Method-call dispatch lives in xexpr_call_method.c (Phase 3 C-02
    // split). When the callee is a member-access expression, every
    // inline optimisation -- StringBuilder / Channel / Map / Coro /
    // INVOKE_DIRECT / chain calls / generic INVOKE -- is handled
    // there and the function returns the result register directly.
    if (node->callee->type == AST_MEMBER_ACCESS) {
        return xr_compile_call_method(ctx, compiler, node, is_tail);
    }


    // Recursion detection: check if calling self
    bool is_recursive = false;

    if (node->callee->type == AST_VARIABLE &&
        compiler->proto->name &&
        compiler->type == FUNCTION_FUNCTION) {

        VariableNode *var = (VariableNode *)&node->callee->as;
        if (strcmp(var->name, compiler->proto->name->data) == 0) {
            is_recursive = true;
        }
    }

    /* Arguments-in-place optimization: detect if arguments are consecutive simple variables.
     *
     * Problem:
     *   select_random(chars, probs) where chars=R[2], probs=R[3]
     *   Before: GETGLOBAL R[13]; MOVE R[14] R[2]; MOVE R[15] R[3]; CALL R[13]
     *
     * Optimization:
     *   If arguments are already in consecutive positions (R[2], R[3]), load function to R[1]
     *   GETGLOBAL R[1]; CALL R[1] (arguments automatically in R[2], R[3])
     *   Save 2 MOVEs!
     */
    int func_reg = -1;
    bool args_in_place = false;

    // Detect consecutive argument optimization conditions
    if (!is_recursive && node->arg_count >= 1 && node->arg_count <= 4) {
        // Check if all arguments are simple variables and consecutively arranged
        int first_arg_reg_expected = -1;
        bool all_vars_consecutive = true;

        for (int i = 0; i < node->arg_count; i++) {
            if (node->arguments[i]->type != AST_VARIABLE) {
                all_vars_consecutive = false;
                break;
            }

            // Get register of variable
            VariableNode *var = &node->arguments[i]->as.variable;
            XrString *name = xr_compile_time_intern(ctx->X, var->name, strlen(var->name));
            int var_reg = scope_resolve_local(compiler, name);

            if (var_reg < 0) {
                // Not a local variable (possibly upvalue or global)
                all_vars_consecutive = false;
                break;
            }

            if (i == 0) {
                first_arg_reg_expected = var_reg;
            } else if (var_reg != first_arg_reg_expected + i) {
                // Not consecutive
                all_vars_consecutive = false;
                break;
            }
        }

        // If arguments are consecutive and there's a free register in front for function
        if (all_vars_consecutive && first_arg_reg_expected >= 1) {
            int candidate_func_reg = first_arg_reg_expected - 1;

            /* Safety check: ensure func_reg won't overwrite existing local variables.
             * func_reg must be >= freereg (unused registers)
             * or be in the temporary register area.
             */
            int current_freereg = xreg_get_freereg(compiler->regalloc);

            // Only safe when target register is in temporary area
            if (candidate_func_reg >= current_freereg) {
                func_reg = candidate_func_reg;
                args_in_place = true;
            }
            // Otherwise don't use this optimization, avoid overwriting existing variables
        }
    }

    if (is_recursive) {
        // Recursive call: no need to load function object
        func_reg = reg_alloc(ctx, compiler);
    } else if (args_in_place) {
        // Arguments-in-place optimization: load function directly to position before arguments
        XrExprDesc callee_desc = xr_compile_expr(ctx, compiler, node->callee);
        xexpr_to_specific_reg(ctx, compiler, &callee_desc, func_reg);
        // Arguments already in correct position, no need for compile_args_to_base
    } else {
        // Regular call: compile callee function expression
        XrExprDesc callee_desc = xr_compile_expr(ctx, compiler, node->callee);
        int callee_reg = xexpr_to_anyreg(ctx, compiler, &callee_desc);

        // Smart register allocation
        if (xexpr_can_reuse_reg(&callee_desc)) {
            func_reg = callee_reg;
        } else {
            func_reg = reg_alloc(ctx, compiler);
            emit_move(compiler->emitter, func_reg, callee_reg);
        }
    }

    // Set freereg = func_reg + 1
    xreg_set_freereg(compiler->regalloc, func_reg + 1);

    // Compile arguments (if no in-place optimization)
    int first_arg_reg = func_reg + 1;
    if (!args_in_place) {
        compile_args_to_base(ctx, compiler, node->arguments, node->arg_count, first_arg_reg);
    }

    // Select optimal instruction based on recursion and tail call status
    if (is_recursive) {
        if (is_tail && compiler->type == FUNCTION_FUNCTION) {
            /* Tail recursion → loop (contification):
             * Single OP_LOOP_BACK replaces CLOSE + N×MOVE + backward JMP.
             * Encoding same as CALLSELF: A=func_reg, B=nargs.
             * VM does: close upvals, memmove R[A+1..A+B] → R[0..B-1], PC=entry.
             */
            emit_abc(compiler->emitter, OP_LOOP_BACK, func_reg, node->arg_count, 0);
            return -1;
        } else {
            // Recursive regular call
            emit_abc(compiler->emitter, OP_CALLSELF, func_reg, node->arg_count, 1);
            xreg_set_freereg(compiler->regalloc, func_reg + 1);
            return func_reg;
        }
    } else if (is_tail && compiler->type == FUNCTION_FUNCTION) {
        // Regular tail call (non-recursive)
        emit_abc(compiler->emitter, OP_TAILCALL, func_reg, node->arg_count, 0);
        return -1;
    } else {
        // Regular call (non-recursive, non-tail call)
        // Use OP_CALL_STATIC when callee is a known function (skip runtime type check)
        bool is_static_call = false;
        if (node->callee->type == AST_VARIABLE) {
            const char *var_name = node->callee->as.variable.name;
            XrString *name_str = xr_compile_time_intern(ctx->X, var_name, strlen(var_name));

            // 1. Local closure variable
            XrLocalInfo *local = compiler_get_local_by_name(compiler, var_name);
            if (local && local->is_closure) {
                is_static_call = true;
            }

            // 2. Shared const function (module-level fn stored in shared_array)
            if (!is_static_call) {
                int sh_idx = shared_get_in_scope(ctx, compiler, name_str);
                if (sh_idx >= 0 && shared_is_const(ctx, sh_idx)) {
                    XrType *ct = shared_get_type(ctx, sh_idx);
                    if (ct && ct->kind == XR_KIND_FUNCTION) {
                        is_static_call = true;
                    }
                }
            }

            // 3. Upvalue-captured function (closure from outer scope)
            // Scan existing upvalues (already resolved during callee compilation)
            if (!is_static_call) {
                int uv_count = PROTO_UPVAL_COUNT(compiler->proto);
                for (int ui = 0; ui < uv_count; ui++) {
                    if (compiler->upvalues[ui].name &&
                        strcmp(XR_STRING_CHARS(compiler->upvalues[ui].name), var_name) == 0) {
                        XrType *ct = compiler->upvalues[ui].type_info;
                        if (ct && ct->kind == XR_KIND_FUNCTION) {
                            is_static_call = true;
                        }
                        break;
                    }
                }
            }
        }
        OpCode call_op = is_static_call ? OP_CALL_STATIC : OP_CALL;
        emit_abc(compiler->emitter, call_op, func_reg, node->arg_count, 1);
        xreg_set_freereg(compiler->regalloc, func_reg + 1);
        return func_reg;
    }
}

/*
 * Internal implementation: compile function call expression (returns register)
 */
static int compile_call_impl(XrCompilerContext *ctx, XrCompiler *compiler, CallExprNode *node) {
    /* First check if it's a higher-order function call (filter/map/reduce).
     * If so, use inline compilation to avoid AST_BLOCK and scope management issues.
     */
    int result_reg;
    if (try_compile_higher_order_call(ctx, compiler, node, &result_reg)) {
        return result_reg;
    }

    // Regular function call
    return compile_call_internal(ctx, compiler, node, false, NULL);
}

/*
 * Compile function call (returns XrExprDesc)
 */
XrExprDesc compile_call(XrCompilerContext *ctx, XrCompiler *compiler, CallExprNode *node) {
    XR_DCHECK(ctx != NULL, "compile_call: NULL ctx");
    XR_DCHECK(compiler != NULL, "compile_call: NULL compiler");
    XR_DCHECK(node != NULL, "compile_call: NULL node");
    XrExprDesc e = {0};
    int reg = compile_call_impl(ctx, compiler, node);

    /* Fix: calls with no return value (like push) return -1, should be VOID type.
     * Avoid subsequent xexpr_to_anyreg trying to move R[-1] (becomes R[255]).
     */
    if (reg < 0) {
        xexpr_init_void(&e);
    } else {
        xexpr_init(&e, XEXPR_CALL, reg);
    }
    return e;
}


