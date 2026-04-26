/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xexpr_higher_order.c - Higher-order function inline compilation
 *
 * KEY CONCEPT:
 *   Directly inline compile filter/map/reduce/forEach at codegen stage.
 *   Replaces AST desugaring approach for better register management.
 *
 * WHY THIS DESIGN:
 *   - No AST transformation, directly generate bytecode
 *   - Proper LIFO register management with batch reclaim
 *   - Supports nested calls and chained operations
 */

#include "../../base/xchecks.h"
#include "xcompiler.h"
#include "xcompiler_context.h"
#include "xexpr.h"
#include "xemit.h"
#include "xregalloc.h"
#include "xexpr_desc.h"
#include "../parser/xast.h"
#include "../../runtime/xisolate_api.h"
#include "../../runtime/symbol/xsymbol_table.h"
#include "../../runtime/value/xtype.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// ========== Helper: Get "length" local symbol index ==========
static int get_length_symbol(XrCompilerContext *ctx, XrCompiler *compiler) {
    int global_sym = xr_symbol_register_in_table(
        (XrSymbolTable *) xr_isolate_get_symbol_table(ctx->X), "length");
    return emitter_add_symbol(compiler->emitter, global_sym);
}

// ========== Helper: Get callback parameter count from AST ==========
static int get_callback_param_count(AstNode *callback_node) {
    if (!callback_node)
        return -1;
    if (callback_node->type == AST_FUNCTION_DECL || callback_node->type == AST_FUNCTION_EXPR) {
        return callback_node->as.function_decl.param_count;
    }
    return -1;  // unknown (variable reference etc.)
}

// ========== Helper: Emit i++ operation ==========
static void emit_increment(XrCompilerContext *ctx, XrCompiler *compiler, int i_reg) {
    int one_kidx = xr_vm_proto_add_constant(compiler->proto, xr_int(1));
    int one_reg = reg_alloc(ctx, compiler);
    emit_loadk(compiler->emitter, one_reg, one_kidx);
    int inc_reg = reg_alloc(ctx, compiler);
    xemit_add(compiler->emitter, inc_reg, i_reg, one_reg);
    emit_move(compiler->emitter, i_reg, inc_reg);
}

// ========== forEach Inline Compilation ==========

/*
 * Inline compile Array.forEach
 *
 * Bytecode pattern:
 *   LOADK     i_reg, 0
 *   GETPROP   len_reg, arr_reg, "size"
 * loop:
 *   CMP_LT    cond_reg, i_reg, len_reg
 *   TEST      cond_reg, 1  ; jump if false
 *   JMP       exit
 *   INDEX_GET val_reg, arr_reg, i_reg
 *   MOVE      arg0, val_reg
 *   MOVE      arg1, i_reg
 *   CALL      callback_reg, 2, 0
 *   ADD       i_reg, i_reg, 1
 *   JMP       loop
 * exit:
 *   LOADNULL  result_reg
 */
static int compile_array_foreach_inline(XrCompilerContext *ctx, XrCompiler *compiler,
                                        CallExprNode *call) {
    MemberAccessNode *member = &call->callee->as.member_access;

    if (call->arg_count < 1) {
        xr_compiler_error(ctx, compiler, "forEach requires a callback argument");
        return reg_alloc(ctx, compiler);
    }

    // Save base for final cleanup
    int base = xreg_get_freereg(compiler->regalloc);

    // 1. Compile array expression
    XrExprDesc arr_expr = xr_compile_expr(ctx, compiler, member->object);
    int arr_reg = xexpr_to_anyreg(ctx, compiler, &arr_expr);

    // 2. Allocate loop control registers
    int i_reg = reg_alloc(ctx, compiler);
    int zero_kidx = xr_vm_proto_add_constant(compiler->proto, xr_int(0));
    emit_loadk(compiler->emitter, i_reg, zero_kidx);

    int len_reg = reg_alloc(ctx, compiler);
    xemit_getprop(compiler->emitter, len_reg, arr_reg, get_length_symbol(ctx, compiler));

    // 3. Compile callback once outside loop
    XrExprDesc callback_expr = xr_compile_expr(ctx, compiler, call->arguments[0]);
    int callback_reg = xexpr_to_anyreg(ctx, compiler, &callback_expr);

    // Mark protected registers
    int loop_base = xreg_get_freereg(compiler->regalloc);

    // Detect callback param count
    int cb_params = get_callback_param_count(call->arguments[0]);
    int call_nargs = (cb_params >= 2) ? 2 : 1;

    // 4. Loop start
    int loop_start = compiler->emitter->pc;

    // Condition: i < len
    xreg_set_freereg(compiler->regalloc, loop_base);
    int cond_reg = reg_alloc(ctx, compiler);
    xemit_cmp_lt(compiler->emitter, cond_reg, i_reg, len_reg);
    xemit_test(compiler->emitter, cond_reg, 0);
    int exit_jump = emit_jump(compiler->emitter, OP_JMP);

    // Loop body
    {
        // Place args directly after callback_reg
        xreg_set_freereg(compiler->regalloc, callback_reg + 1);
        int arg0 = xreg_alloc_next(compiler->regalloc);
        xemit_index_get(compiler->emitter, arg0, arr_reg, i_reg);
        if (call_nargs >= 2) {
            int arg1 = xreg_alloc_next(compiler->regalloc);
            emit_move(compiler->emitter, arg1, i_reg);
        }
        int discard_reg = xreg_alloc_next(compiler->regalloc);

        // CALL_KEEP: call callback, result to discard_reg, callback_reg preserved
        xemit_call_keep(compiler->emitter, callback_reg, call_nargs, discard_reg);
    }

    // i++
    xreg_set_freereg(compiler->regalloc, loop_base);
    emit_increment(ctx, compiler, i_reg);

    // Jump back to loop start
    int offset = loop_start - (compiler->emitter->pc + 1);
    emit_sj(compiler->emitter, OP_JMP, offset);

    // Exit
    patch_jump(compiler->emitter, exit_jump, -1);

    // 5. forEach returns null
    xreg_set_freereg(compiler->regalloc, base);
    int result_reg = reg_alloc(ctx, compiler);
    xemit_loadnull(compiler->emitter, result_reg);

    return result_reg;
}

// ========== Filter Inline Compilation ==========

/*
 * Inline compile Array.filter
 *
 * Bytecode pattern:
 *   NEWARRAY  result_reg, 0
 *   LOADK     idx_reg, 0
 *   LOADK     i_reg, 0
 *   GETPROP   len_reg, arr_reg, "size"
 * loop:
 *   CMP_LT    cond_reg, i_reg, len_reg
 *   TEST      cond_reg, 1
 *   JMP       exit
 *   INDEX_GET val_reg, arr_reg, i_reg
 *   CALL      callback_reg(val, i) -> callback_reg
 *   TEST      callback_reg, 1
 *   JMP       skip
 *   INDEX_SET result_reg, idx_reg, val_reg
 *   ADD       idx_reg, idx_reg, 1
 * skip:
 *   ADD       i_reg, i_reg, 1
 *   JMP       loop
 * exit:
 */
static int compile_array_filter_inline(XrCompilerContext *ctx, XrCompiler *compiler,
                                       CallExprNode *call) {
    MemberAccessNode *member = &call->callee->as.member_access;

    if (call->arg_count < 1) {
        xr_compiler_error(ctx, compiler, "filter requires a callback argument");
        return reg_alloc(ctx, compiler);
    }

    int base = xreg_get_freereg(compiler->regalloc);

    // 1. Compile array
    XrExprDesc arr_expr = xr_compile_expr(ctx, compiler, member->object);
    int arr_reg = xexpr_to_anyreg(ctx, compiler, &arr_expr);

    // 2. Allocate control registers
    int result_reg = reg_alloc(ctx, compiler);
    int c_arr_f = ((int) ctx->current_elem_tid << 2) | ctx->current_storage_mode;
    xemit_newarray(compiler->emitter, result_reg, 0, c_arr_f);

    int i_reg = reg_alloc(ctx, compiler);
    int zero_kidx = xr_vm_proto_add_constant(compiler->proto, xr_int(0));
    emit_loadk(compiler->emitter, i_reg, zero_kidx);

    int len_reg = reg_alloc(ctx, compiler);
    xemit_getprop(compiler->emitter, len_reg, arr_reg, get_length_symbol(ctx, compiler));

    // 3. Compile callback
    XrExprDesc callback_expr = xr_compile_expr(ctx, compiler, call->arguments[0]);
    int callback_reg = xexpr_to_anyreg(ctx, compiler, &callback_expr);

    int loop_base = xreg_get_freereg(compiler->regalloc);

    // Detect callback param count
    int cb_params_f = get_callback_param_count(call->arguments[0]);
    int call_nargs_f = (cb_params_f >= 2) ? 2 : 1;

    // 4. Loop
    int loop_start = compiler->emitter->pc;

    xreg_set_freereg(compiler->regalloc, loop_base);
    int cond_reg = reg_alloc(ctx, compiler);
    xemit_cmp_lt(compiler->emitter, cond_reg, i_reg, len_reg);
    xemit_test(compiler->emitter, cond_reg, 0);
    int exit_jump = emit_jump(compiler->emitter, OP_JMP);

    // Loop body
    {
        // Place args after callback_reg: arg0 = arr[i]
        xreg_set_freereg(compiler->regalloc, callback_reg + 1);
        int arg0 = xreg_alloc_next(compiler->regalloc);
        xemit_index_get(compiler->emitter, arg0, arr_reg, i_reg);
        if (call_nargs_f >= 2) {
            int arg1 = xreg_alloc_next(compiler->regalloc);
            emit_move(compiler->emitter, arg1, i_reg);
        }
        int call_result = xreg_alloc_next(compiler->regalloc);

        // CALL_KEEP: callback preserved, result in call_result
        xemit_call_keep(compiler->emitter, callback_reg, call_nargs_f, call_result);

        // if (call_result is truthy), include element
        xemit_test(compiler->emitter, call_result, 0);
        int skip_jump = emit_jump(compiler->emitter, OP_JMP);

        // Re-read val (callee's stack frame may have overwritten earlier registers)
        int val_reg = xreg_alloc_next(compiler->regalloc);
        xemit_index_get(compiler->emitter, val_reg, arr_reg, i_reg);
        xemit_array_push(compiler->emitter, result_reg, val_reg);

        patch_jump(compiler->emitter, skip_jump, -1);
    }

    // i++
    xreg_set_freereg(compiler->regalloc, loop_base);
    emit_increment(ctx, compiler, i_reg);

    int offset = loop_start - (compiler->emitter->pc + 1);
    emit_sj(compiler->emitter, OP_JMP, offset);

    patch_jump(compiler->emitter, exit_jump, -1);

    // 5. Cleanup and return result
    xreg_set_freereg(compiler->regalloc, base);
    int final_reg = reg_alloc(ctx, compiler);
    if (final_reg != result_reg) {
        emit_move(compiler->emitter, final_reg, result_reg);
    }
    return final_reg;
}

// ========== Map Inline Compilation ==========

/*
 * Inline compile Array.map
 *
 * Bytecode pattern:
 *   NEWARRAY  result_reg, 0
 *   LOADK     i_reg, 0
 *   GETPROP   len_reg, arr_reg, "size"
 * loop:
 *   CMP_LT    cond_reg, i_reg, len_reg
 *   TEST      cond_reg, 1
 *   JMP       exit
 *   INDEX_GET val_reg, arr_reg, i_reg
 *   CALL      callback_reg(val, i) -> callback_reg
 *   INDEX_SET result_reg, i_reg, callback_reg
 *   ADD       i_reg, i_reg, 1
 *   JMP       loop
 * exit:
 */
static int compile_array_map_inline(XrCompilerContext *ctx, XrCompiler *compiler,
                                    CallExprNode *call) {
    MemberAccessNode *member = &call->callee->as.member_access;

    if (call->arg_count < 1) {
        xr_compiler_error(ctx, compiler, "map requires a callback argument");
        return reg_alloc(ctx, compiler);
    }

    int base = xreg_get_freereg(compiler->regalloc);

    // 1. Compile array
    XrExprDesc arr_expr = xr_compile_expr(ctx, compiler, member->object);
    int arr_reg = xexpr_to_anyreg(ctx, compiler, &arr_expr);

    // 2. Allocate control registers
    int result_reg = reg_alloc(ctx, compiler);
    int c_arr_m = ((int) ctx->current_elem_tid << 2) | ctx->current_storage_mode;
    xemit_newarray(compiler->emitter, result_reg, 0, c_arr_m);

    int i_reg = reg_alloc(ctx, compiler);
    int zero_kidx = xr_vm_proto_add_constant(compiler->proto, xr_int(0));
    emit_loadk(compiler->emitter, i_reg, zero_kidx);

    int len_reg = reg_alloc(ctx, compiler);
    xemit_getprop(compiler->emitter, len_reg, arr_reg, get_length_symbol(ctx, compiler));

    // 3. Compile callback
    XrExprDesc callback_expr = xr_compile_expr(ctx, compiler, call->arguments[0]);
    int callback_reg = xexpr_to_anyreg(ctx, compiler, &callback_expr);

    int loop_base = xreg_get_freereg(compiler->regalloc);

    // Detect callback param count
    int cb_params_m = get_callback_param_count(call->arguments[0]);
    int call_nargs_m = (cb_params_m >= 2) ? 2 : 1;

    // 4. Loop
    int loop_start = compiler->emitter->pc;

    xreg_set_freereg(compiler->regalloc, loop_base);
    int cond_reg = reg_alloc(ctx, compiler);
    xemit_cmp_lt(compiler->emitter, cond_reg, i_reg, len_reg);
    xemit_test(compiler->emitter, cond_reg, 0);
    int exit_jump = emit_jump(compiler->emitter, OP_JMP);

    // Loop body
    {
        // Place args after callback_reg
        xreg_set_freereg(compiler->regalloc, callback_reg + 1);
        int arg0 = xreg_alloc_next(compiler->regalloc);
        xemit_index_get(compiler->emitter, arg0, arr_reg, i_reg);
        if (call_nargs_m >= 2) {
            int arg1 = xreg_alloc_next(compiler->regalloc);
            emit_move(compiler->emitter, arg1, i_reg);
        }
        int call_result = xreg_alloc_next(compiler->regalloc);

        // CALL_KEEP: callback preserved, result in call_result
        xemit_call_keep(compiler->emitter, callback_reg, call_nargs_m, call_result);

        // result.push(mapped_value)
        xemit_array_push(compiler->emitter, result_reg, call_result);
    }

    // i++
    xreg_set_freereg(compiler->regalloc, loop_base);
    emit_increment(ctx, compiler, i_reg);

    int offset = loop_start - (compiler->emitter->pc + 1);
    emit_sj(compiler->emitter, OP_JMP, offset);

    patch_jump(compiler->emitter, exit_jump, -1);

    // 5. Cleanup and return result
    xreg_set_freereg(compiler->regalloc, base);
    int final_reg = reg_alloc(ctx, compiler);
    if (final_reg != result_reg) {
        emit_move(compiler->emitter, final_reg, result_reg);
    }
    return final_reg;
}

// ========== Reduce Inline Compilation ==========

/*
 * Inline compile Array.reduce
 *
 * Bytecode pattern:
 *   MOVE      acc_reg, initial_value
 *   LOADK     i_reg, 0
 *   GETPROP   len_reg, arr_reg, "size"
 * loop:
 *   CMP_LT    cond_reg, i_reg, len_reg
 *   TEST      cond_reg, 1
 *   JMP       exit
 *   INDEX_GET val_reg, arr_reg, i_reg
 *   CALL      callback_reg(acc, val, i) -> callback_reg
 *   MOVE      acc_reg, callback_reg
 *   ADD       i_reg, i_reg, 1
 *   JMP       loop
 * exit:
 */
static int compile_array_reduce_inline(XrCompilerContext *ctx, XrCompiler *compiler,
                                       CallExprNode *call) {
    MemberAccessNode *member = &call->callee->as.member_access;

    if (call->arg_count < 2) {
        xr_compiler_error(ctx, compiler, "reduce requires callback and initial value");
        return reg_alloc(ctx, compiler);
    }

    int base = xreg_get_freereg(compiler->regalloc);

    // 1. Compile array
    XrExprDesc arr_expr = xr_compile_expr(ctx, compiler, member->object);
    int arr_reg = xexpr_to_anyreg(ctx, compiler, &arr_expr);

    // 2. Compile initial value
    XrExprDesc init_expr = xr_compile_expr(ctx, compiler, call->arguments[1]);
    int acc_reg = xexpr_to_anyreg(ctx, compiler, &init_expr);

    // 3. Allocate control registers
    int i_reg = reg_alloc(ctx, compiler);
    int zero_kidx = xr_vm_proto_add_constant(compiler->proto, xr_int(0));
    emit_loadk(compiler->emitter, i_reg, zero_kidx);

    int len_reg = reg_alloc(ctx, compiler);
    xemit_getprop(compiler->emitter, len_reg, arr_reg, get_length_symbol(ctx, compiler));

    // 4. Compile callback
    XrExprDesc callback_expr = xr_compile_expr(ctx, compiler, call->arguments[0]);
    int callback_reg = xexpr_to_anyreg(ctx, compiler, &callback_expr);

    int loop_base = xreg_get_freereg(compiler->regalloc);

    // Detect callback param count for reduce
    int cb_params_r = get_callback_param_count(call->arguments[0]);
    int call_nargs_r = (cb_params_r >= 3) ? 3 : 2;

    // 5. Loop
    int loop_start = compiler->emitter->pc;

    xreg_set_freereg(compiler->regalloc, loop_base);
    int cond_reg = reg_alloc(ctx, compiler);
    xemit_cmp_lt(compiler->emitter, cond_reg, i_reg, len_reg);
    xemit_test(compiler->emitter, cond_reg, 0);
    int exit_jump = emit_jump(compiler->emitter, OP_JMP);

    // Loop body
    {
        // Place args after callback_reg: callback(acc, val[, i])
        xreg_set_freereg(compiler->regalloc, callback_reg + 1);
        int arg0 = xreg_alloc_next(compiler->regalloc);
        emit_move(compiler->emitter, arg0, acc_reg);
        int arg1 = xreg_alloc_next(compiler->regalloc);
        xemit_index_get(compiler->emitter, arg1, arr_reg, i_reg);
        if (call_nargs_r >= 3) {
            int arg2 = xreg_alloc_next(compiler->regalloc);
            emit_move(compiler->emitter, arg2, i_reg);
        }
        int call_result = xreg_alloc_next(compiler->regalloc);

        // CALL_KEEP: callback preserved, result in call_result
        xemit_call_keep(compiler->emitter, callback_reg, call_nargs_r, call_result);

        // acc = callback result
        emit_move(compiler->emitter, acc_reg, call_result);
    }

    // i++
    xreg_set_freereg(compiler->regalloc, loop_base);
    emit_increment(ctx, compiler, i_reg);

    int offset = loop_start - (compiler->emitter->pc + 1);
    emit_sj(compiler->emitter, OP_JMP, offset);

    patch_jump(compiler->emitter, exit_jump, -1);

    // 6. Cleanup and return accumulator
    xreg_set_freereg(compiler->regalloc, base);
    int final_reg = reg_alloc(ctx, compiler);
    if (final_reg != acc_reg) {
        emit_move(compiler->emitter, final_reg, acc_reg);
    }
    return final_reg;
}

// ========== Helper: Get "keys" local symbol index ==========
static int get_keys_symbol(XrCompilerContext *ctx, XrCompiler *compiler) {
    int global_sym =
        xr_symbol_register_in_table((XrSymbolTable *) xr_isolate_get_symbol_table(ctx->X), "keys");
    return emitter_add_symbol(compiler->emitter, global_sym);
}

// ========== Map forEach Inline Compilation ==========

/*
 * Inline compile Map.forEach
 *
 * Bytecode pattern:
 *   GETPROP   keys_reg, map_reg, "keys"
 *   LOADK     i_reg, 0
 *   GETPROP   len_reg, keys_reg, "size"
 * loop:
 *   CMP_LT    cond_reg, i_reg, len_reg
 *   TEST      cond_reg, 1
 *   JMP       exit
 *   INDEX_GET key_reg, keys_reg, i_reg
 *   INDEX_GET val_reg, map_reg, key_reg
 *   CALL      callback_reg(val, key)
 *   ADD       i_reg, i_reg, 1
 *   JMP       loop
 * exit:
 *   LOADNULL  result_reg
 */
static int compile_map_foreach_inline(XrCompilerContext *ctx, XrCompiler *compiler,
                                      CallExprNode *call) {
    MemberAccessNode *member = &call->callee->as.member_access;

    if (call->arg_count < 1) {
        xr_compiler_error(ctx, compiler, "forEach requires a callback argument");
        return reg_alloc(ctx, compiler);
    }

    int base = xreg_get_freereg(compiler->regalloc);

    // 1. Compile map expression
    XrExprDesc map_expr = xr_compile_expr(ctx, compiler, member->object);
    int map_reg = xexpr_to_anyreg(ctx, compiler, &map_expr);

    // 2. Get keys array: keys = map.keys
    int keys_reg = reg_alloc(ctx, compiler);
    xemit_getprop(compiler->emitter, keys_reg, map_reg, get_keys_symbol(ctx, compiler));

    // 3. Allocate loop control registers
    int i_reg = reg_alloc(ctx, compiler);
    int zero_kidx = xr_vm_proto_add_constant(compiler->proto, xr_int(0));
    emit_loadk(compiler->emitter, i_reg, zero_kidx);

    int len_reg = reg_alloc(ctx, compiler);
    xemit_getprop(compiler->emitter, len_reg, keys_reg, get_length_symbol(ctx, compiler));

    // 4. Compile callback
    XrExprDesc callback_expr = xr_compile_expr(ctx, compiler, call->arguments[0]);
    int callback_reg = xexpr_to_anyreg(ctx, compiler, &callback_expr);

    int loop_base = xreg_get_freereg(compiler->regalloc);

    // 5. Loop
    int loop_start = compiler->emitter->pc;

    xreg_set_freereg(compiler->regalloc, loop_base);
    int cond_reg = reg_alloc(ctx, compiler);
    xemit_cmp_lt(compiler->emitter, cond_reg, i_reg, len_reg);
    xemit_test(compiler->emitter, cond_reg, 0);
    int exit_jump = emit_jump(compiler->emitter, OP_JMP);

    // Loop body
    {
        int body_base = xreg_get_freereg(compiler->regalloc);

        // key = keys[i], val = map[key] (need both for callback args)
        int key_reg = reg_alloc(ctx, compiler);
        xemit_index_get(compiler->emitter, key_reg, keys_reg, i_reg);
        int val_reg = reg_alloc(ctx, compiler);
        xemit_index_get(compiler->emitter, val_reg, map_reg, key_reg);

        // Place args after callback_reg: callback(val, key)
        xreg_set_freereg(compiler->regalloc, callback_reg + 1);
        int arg0 = xreg_alloc_next(compiler->regalloc);
        emit_move(compiler->emitter, arg0, val_reg);
        int arg1 = xreg_alloc_next(compiler->regalloc);
        emit_move(compiler->emitter, arg1, key_reg);
        int discard_reg = xreg_alloc_next(compiler->regalloc);

        // CALL_KEEP: callback preserved
        xemit_call_keep(compiler->emitter, callback_reg, 2, discard_reg);

        xreg_set_freereg(compiler->regalloc, body_base);
    }

    // i++
    xreg_set_freereg(compiler->regalloc, loop_base);
    emit_increment(ctx, compiler, i_reg);

    int offset = loop_start - (compiler->emitter->pc + 1);
    emit_sj(compiler->emitter, OP_JMP, offset);

    patch_jump(compiler->emitter, exit_jump, -1);

    // 6. forEach returns null
    xreg_set_freereg(compiler->regalloc, base);
    int result_reg = reg_alloc(ctx, compiler);
    xemit_loadnull(compiler->emitter, result_reg);

    return result_reg;
}

// ========== Map map Inline Compilation ==========

/*
 * Inline compile Map.map
 *
 * Returns a new Map with transformed values.
 */
static int compile_map_map_inline(XrCompilerContext *ctx, XrCompiler *compiler,
                                  CallExprNode *call) {
    MemberAccessNode *member = &call->callee->as.member_access;

    if (call->arg_count < 1) {
        xr_compiler_error(ctx, compiler, "map requires a callback argument");
        return reg_alloc(ctx, compiler);
    }

    int base = xreg_get_freereg(compiler->regalloc);

    // 1. Compile map expression
    XrExprDesc map_expr = xr_compile_expr(ctx, compiler, member->object);
    int map_reg = xexpr_to_anyreg(ctx, compiler, &map_expr);

    // 2. Create result map: result = #{}
    int result_reg = reg_alloc(ctx, compiler);
    int ck_map = (ctx->current_key_tid == XR_TID_STRING) ? 1
                 : (ctx->current_key_tid == XR_TID_INT)  ? 2
                                                         : 0;
    int c_map =
        (ck_map << 7) | (((int) ctx->current_elem_tid & 0x1F) << 2) | ctx->current_storage_mode;
    xemit_newmap(compiler->emitter, result_reg, 0, c_map);

    // 3. Get keys array
    int keys_reg = reg_alloc(ctx, compiler);
    xemit_getprop(compiler->emitter, keys_reg, map_reg, get_keys_symbol(ctx, compiler));

    // 4. Loop control
    int i_reg = reg_alloc(ctx, compiler);
    int zero_kidx = xr_vm_proto_add_constant(compiler->proto, xr_int(0));
    emit_loadk(compiler->emitter, i_reg, zero_kidx);

    int len_reg = reg_alloc(ctx, compiler);
    xemit_getprop(compiler->emitter, len_reg, keys_reg, get_length_symbol(ctx, compiler));

    // 5. Compile callback
    XrExprDesc callback_expr = xr_compile_expr(ctx, compiler, call->arguments[0]);
    int callback_reg = xexpr_to_anyreg(ctx, compiler, &callback_expr);

    int loop_base = xreg_get_freereg(compiler->regalloc);

    // 6. Loop
    int loop_start = compiler->emitter->pc;

    xreg_set_freereg(compiler->regalloc, loop_base);
    int cond_reg = reg_alloc(ctx, compiler);
    xemit_cmp_lt(compiler->emitter, cond_reg, i_reg, len_reg);
    xemit_test(compiler->emitter, cond_reg, 0);
    int exit_jump = emit_jump(compiler->emitter, OP_JMP);

    {
        int body_base = xreg_get_freereg(compiler->regalloc);

        // key = keys[i] (needed for INDEX_SET later)
        int key_reg = reg_alloc(ctx, compiler);
        xemit_index_get(compiler->emitter, key_reg, keys_reg, i_reg);

        // val = map[key]
        int val_reg = reg_alloc(ctx, compiler);
        xemit_index_get(compiler->emitter, val_reg, map_reg, key_reg);

        // Place args after callback_reg: callback(val, key)
        xreg_set_freereg(compiler->regalloc, callback_reg + 1);
        int arg0 = xreg_alloc_next(compiler->regalloc);
        emit_move(compiler->emitter, arg0, val_reg);
        int arg1 = xreg_alloc_next(compiler->regalloc);
        emit_move(compiler->emitter, arg1, key_reg);
        int call_result = xreg_alloc_next(compiler->regalloc);

        // CALL_KEEP: callback preserved, result in call_result
        xemit_call_keep(compiler->emitter, callback_reg, 2, call_result);

        // result[key] = new_val
        xemit_index_set(compiler->emitter, result_reg, key_reg, call_result);

        xreg_set_freereg(compiler->regalloc, body_base);
    }

    xreg_set_freereg(compiler->regalloc, loop_base);
    emit_increment(ctx, compiler, i_reg);

    int offset = loop_start - (compiler->emitter->pc + 1);
    emit_sj(compiler->emitter, OP_JMP, offset);

    patch_jump(compiler->emitter, exit_jump, -1);

    // 7. Cleanup and return result
    xreg_set_freereg(compiler->regalloc, base);
    int final_reg = reg_alloc(ctx, compiler);
    if (final_reg != result_reg) {
        emit_move(compiler->emitter, final_reg, result_reg);
    }
    return final_reg;
}

// ========== Map filter Inline Compilation ==========

/*
 * Inline compile Map.filter
 *
 * Returns a new Map with filtered key-value pairs.
 */
static int compile_map_filter_inline(XrCompilerContext *ctx, XrCompiler *compiler,
                                     CallExprNode *call) {
    MemberAccessNode *member = &call->callee->as.member_access;

    if (call->arg_count < 1) {
        xr_compiler_error(ctx, compiler, "filter requires a callback argument");
        return reg_alloc(ctx, compiler);
    }

    int base = xreg_get_freereg(compiler->regalloc);

    // 1. Compile map expression
    XrExprDesc map_expr = xr_compile_expr(ctx, compiler, member->object);
    int map_reg = xexpr_to_anyreg(ctx, compiler, &map_expr);

    // 2. Create result map
    int result_reg = reg_alloc(ctx, compiler);
    int ck_map2 = (ctx->current_key_tid == XR_TID_STRING) ? 1
                  : (ctx->current_key_tid == XR_TID_INT)  ? 2
                                                          : 0;
    int c_map2 =
        (ck_map2 << 7) | (((int) ctx->current_elem_tid & 0x1F) << 2) | ctx->current_storage_mode;
    xemit_newmap(compiler->emitter, result_reg, 0, c_map2);

    // 3. Get keys array
    int keys_reg = reg_alloc(ctx, compiler);
    xemit_getprop(compiler->emitter, keys_reg, map_reg, get_keys_symbol(ctx, compiler));

    // 4. Loop control
    int i_reg = reg_alloc(ctx, compiler);
    int zero_kidx = xr_vm_proto_add_constant(compiler->proto, xr_int(0));
    emit_loadk(compiler->emitter, i_reg, zero_kidx);

    int len_reg = reg_alloc(ctx, compiler);
    xemit_getprop(compiler->emitter, len_reg, keys_reg, get_length_symbol(ctx, compiler));

    // 5. Compile callback
    XrExprDesc callback_expr = xr_compile_expr(ctx, compiler, call->arguments[0]);
    int callback_reg = xexpr_to_anyreg(ctx, compiler, &callback_expr);

    int loop_base = xreg_get_freereg(compiler->regalloc);

    // 6. Loop
    int loop_start = compiler->emitter->pc;

    xreg_set_freereg(compiler->regalloc, loop_base);
    int cond_reg = reg_alloc(ctx, compiler);
    xemit_cmp_lt(compiler->emitter, cond_reg, i_reg, len_reg);
    xemit_test(compiler->emitter, cond_reg, 0);
    int exit_jump = emit_jump(compiler->emitter, OP_JMP);

    {
        int body_base = xreg_get_freereg(compiler->regalloc);

        // key = keys[i], val = map[key] (need both for INDEX_SET)
        int key_reg = reg_alloc(ctx, compiler);
        xemit_index_get(compiler->emitter, key_reg, keys_reg, i_reg);
        int val_reg = reg_alloc(ctx, compiler);
        xemit_index_get(compiler->emitter, val_reg, map_reg, key_reg);

        // Place args after callback_reg: callback(val, key)
        xreg_set_freereg(compiler->regalloc, callback_reg + 1);
        int arg0 = xreg_alloc_next(compiler->regalloc);
        emit_move(compiler->emitter, arg0, val_reg);
        int arg1 = xreg_alloc_next(compiler->regalloc);
        emit_move(compiler->emitter, arg1, key_reg);
        int call_result = xreg_alloc_next(compiler->regalloc);

        // CALL_KEEP: callback preserved, result in call_result
        xemit_call_keep(compiler->emitter, callback_reg, 2, call_result);

        // if (call_result is truthy), include element
        xemit_test(compiler->emitter, call_result, 0);
        int skip_jump = emit_jump(compiler->emitter, OP_JMP);
        xemit_index_set(compiler->emitter, result_reg, key_reg, val_reg);
        patch_jump(compiler->emitter, skip_jump, -1);

        xreg_set_freereg(compiler->regalloc, body_base);
    }

    xreg_set_freereg(compiler->regalloc, loop_base);
    emit_increment(ctx, compiler, i_reg);

    int offset = loop_start - (compiler->emitter->pc + 1);
    emit_sj(compiler->emitter, OP_JMP, offset);

    patch_jump(compiler->emitter, exit_jump, -1);

    // 7. Cleanup and return result
    xreg_set_freereg(compiler->regalloc, base);
    int final_reg = reg_alloc(ctx, compiler);
    if (final_reg != result_reg) {
        emit_move(compiler->emitter, final_reg, result_reg);
    }
    return final_reg;
}

// ========== Map reduce Inline Compilation ==========

/*
 * Inline compile Map.reduce
 */
static int compile_map_reduce_inline(XrCompilerContext *ctx, XrCompiler *compiler,
                                     CallExprNode *call) {
    MemberAccessNode *member = &call->callee->as.member_access;

    if (call->arg_count < 2) {
        xr_compiler_error(ctx, compiler, "reduce requires callback and initial value");
        return reg_alloc(ctx, compiler);
    }

    int base = xreg_get_freereg(compiler->regalloc);

    // 1. Compile map expression
    XrExprDesc map_expr = xr_compile_expr(ctx, compiler, member->object);
    int map_reg = xexpr_to_anyreg(ctx, compiler, &map_expr);

    // 2. Compile initial value
    XrExprDesc init_expr = xr_compile_expr(ctx, compiler, call->arguments[1]);
    int acc_reg = xexpr_to_anyreg(ctx, compiler, &init_expr);

    // 3. Get keys array
    int keys_reg = reg_alloc(ctx, compiler);
    xemit_getprop(compiler->emitter, keys_reg, map_reg, get_keys_symbol(ctx, compiler));

    // 4. Loop control
    int i_reg = reg_alloc(ctx, compiler);
    int zero_kidx = xr_vm_proto_add_constant(compiler->proto, xr_int(0));
    emit_loadk(compiler->emitter, i_reg, zero_kidx);

    int len_reg = reg_alloc(ctx, compiler);
    xemit_getprop(compiler->emitter, len_reg, keys_reg, get_length_symbol(ctx, compiler));

    // 5. Compile callback
    XrExprDesc callback_expr = xr_compile_expr(ctx, compiler, call->arguments[0]);
    int callback_reg = xexpr_to_anyreg(ctx, compiler, &callback_expr);

    int loop_base = xreg_get_freereg(compiler->regalloc);

    // 6. Loop
    int loop_start = compiler->emitter->pc;

    xreg_set_freereg(compiler->regalloc, loop_base);
    int cond_reg = reg_alloc(ctx, compiler);
    xemit_cmp_lt(compiler->emitter, cond_reg, i_reg, len_reg);
    xemit_test(compiler->emitter, cond_reg, 0);
    int exit_jump = emit_jump(compiler->emitter, OP_JMP);

    {
        // Place args after callback_reg: callback(acc, val, key)
        xreg_set_freereg(compiler->regalloc, callback_reg + 1);
        int arg0 = xreg_alloc_next(compiler->regalloc);
        emit_move(compiler->emitter, arg0, acc_reg);

        // key = keys[i]
        int key_reg = reg_alloc(ctx, compiler);
        xemit_index_get(compiler->emitter, key_reg, keys_reg, i_reg);

        // val = map[key] -> arg1
        int arg1 = xreg_alloc_next(compiler->regalloc);
        xemit_index_get(compiler->emitter, arg1, map_reg, key_reg);

        int arg2 = xreg_alloc_next(compiler->regalloc);
        emit_move(compiler->emitter, arg2, key_reg);
        int call_result = xreg_alloc_next(compiler->regalloc);

        // CALL_KEEP: callback preserved, result in call_result
        xemit_call_keep(compiler->emitter, callback_reg, 3, call_result);

        // acc = callback result
        emit_move(compiler->emitter, acc_reg, call_result);
    }

    xreg_set_freereg(compiler->regalloc, loop_base);
    emit_increment(ctx, compiler, i_reg);

    int offset = loop_start - (compiler->emitter->pc + 1);
    emit_sj(compiler->emitter, OP_JMP, offset);

    patch_jump(compiler->emitter, exit_jump, -1);

    // 7. Cleanup and return accumulator
    xreg_set_freereg(compiler->regalloc, base);
    int final_reg = reg_alloc(ctx, compiler);
    if (final_reg != acc_reg) {
        emit_move(compiler->emitter, final_reg, acc_reg);
    }
    return final_reg;
}

// ========== Entry Function ==========

/*
 * Try to inline compile higher-order function call
 *
 * Handles Array/Map forEach/map/filter/reduce with inline bytecode generation.
 * For unknown type, falls back to runtime dispatch.
 *
 * @return true if handled, false for runtime method call
 */
bool try_compile_higher_order_call(XrCompilerContext *ctx, XrCompiler *compiler, CallExprNode *call,
                                   int *result_reg) {
    if (!call || !call->callee)
        return false;
    if (call->callee->type != AST_MEMBER_ACCESS)
        return false;

    MemberAccessNode *member = &call->callee->as.member_access;
    if (!member->name)
        return false;

    // Check method name first
    bool is_forEach = strcmp(member->name, "forEach") == 0;
    bool is_map = strcmp(member->name, "map") == 0;
    bool is_filter = strcmp(member->name, "filter") == 0;
    bool is_reduce = strcmp(member->name, "reduce") == 0;

    if (!is_forEach && !is_map && !is_filter && !is_reduce) {
        return false;
    }

    // Read the inferred type via the analyzer side table.
    XrType *obj_type = xa_analyzer_get_node_type(ctx->analyzer, member->object);

    // Fallback: infer type from expression if compile_type is not set
    if (!obj_type || XR_TYPE_IS_UNKNOWN(obj_type)) {
        XrType *inferred = get_expr_type(ctx, compiler, member->object);
        if (inferred && !XR_TYPE_IS_UNKNOWN(inferred)) {
            obj_type = inferred;
        }
    }

    // Array methods (optimized iteration)
    if (obj_type && (obj_type->kind == XR_KIND_ARRAY)) {
        if (is_forEach) {
            *result_reg = compile_array_foreach_inline(ctx, compiler, call);
            return true;
        } else if (is_map) {
            *result_reg = compile_array_map_inline(ctx, compiler, call);
            return true;
        } else if (is_filter) {
            *result_reg = compile_array_filter_inline(ctx, compiler, call);
            return true;
        } else if (is_reduce) {
            *result_reg = compile_array_reduce_inline(ctx, compiler, call);
            return true;
        }
    }
    // Map methods
    else if (obj_type && (obj_type->kind == XR_KIND_MAP)) {
        if (is_forEach) {
            *result_reg = compile_map_foreach_inline(ctx, compiler, call);
            return true;
        } else if (is_map) {
            *result_reg = compile_map_map_inline(ctx, compiler, call);
            return true;
        } else if (is_filter) {
            *result_reg = compile_map_filter_inline(ctx, compiler, call);
            return true;
        } else if (is_reduce) {
            *result_reg = compile_map_reduce_inline(ctx, compiler, call);
            return true;
        }
    }
    // Unknown type: fall back to runtime dispatch
    // Map-style iteration has different arg count (3 vs 2 for Array),
    // so inlining for unknown types would pass wrong args to callbacks

    return false;
}
