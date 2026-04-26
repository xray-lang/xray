/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xexpr_call_builtin.c - Builtin function call compilation
 *
 * Holds the builtin-call dispatcher subset of xexpr_call.c. Contains:
 *   - the per-builtin compile_builtin_* helpers
 *   - the binary-searched builtin_functions[] table
 *   - the xr_compile_call_builtin() dispatcher (which also handles
 *     the implicit RETURN1 emit for tail-position builtin calls)
 *
 * Public surface: xr_compile_call_builtin only. Everything else is
 * file-static.
 */

#include "xexpr_call_builtin.h"
#include "xexpr.h"
#include "../../base/xchecks.h"
#include "xexpr_desc.h"
#include "xcompiler.h"
#include "xcompiler_context.h"
#include "xemit.h"
#include "xregalloc.h"
#include "../parser/xast.h"
#include "../../runtime/xisolate_api.h"
#include <stdio.h>
#include <string.h>

/* ========== Helper Functions ========== */

/*
 * Compile unary builtin function: op result, arg
 * Pattern: compile arg -> allocate result -> emit instruction -> return result register
 */
static int compile_unary_builtin(XrCompilerContext *ctx, XrCompiler *compiler,
                                  AstNode *arg, OpCode op) {
    XR_DCHECK(ctx != NULL, "compile_unary_builtin: NULL ctx");
    XR_DCHECK(compiler != NULL, "compile_unary_builtin: NULL compiler");
    XrExprDesc arg_desc = xr_compile_expr(ctx, compiler, arg);
    int arg_reg = xexpr_to_anyreg(ctx, compiler, &arg_desc);
    int result_reg = reg_alloc(ctx, compiler);
    emit_abc(compiler->emitter, op, result_reg, arg_reg, 0);
    xreg_set_freereg(compiler->regalloc, result_reg + 1);  // Batch reclaim
    return result_reg;
}

static int compile_builtin_func(XrCompilerContext *ctx, XrCompiler *compiler,
                                 AstNode *arg, OpCode op) {
    return compile_unary_builtin(ctx, compiler, arg, op);
}


/* ========== Builtin Function Table ========== */

// Return value: register number, or BUILTIN_NOT_HANDLED meaning
// "matched name but bailed (e.g. arity mismatch); fall through".
#define BUILTIN_NOT_HANDLED (-2)

typedef int (*BuiltinCompileFn)(XrCompilerContext *ctx, XrCompiler *compiler, CallExprNode *node);

typedef struct {
    const char *name;
    BuiltinCompileFn compile;
} BuiltinEntry;

static int compile_builtin_assert(XrCompilerContext *ctx, XrCompiler *compiler, CallExprNode *node) {
    XR_DCHECK(ctx != NULL, "compile_builtin_assert: NULL ctx");
    XR_DCHECK(compiler != NULL, "compile_builtin_assert: NULL compiler");
    if (node->arg_count == 1 || node->arg_count == 2) {
        XrExprDesc cond_desc = xr_compile_expr(ctx, compiler, node->arguments[0]);
        int cond_reg = xexpr_to_anyreg(ctx, compiler, &cond_desc);
        char loc_buf[256];
        if (node->arg_count == 2 && node->arguments[1]->type == AST_LITERAL_STRING) {
            snprintf(loc_buf, sizeof(loc_buf), "line %d: %s",
                     node->arguments[0]->line, node->arguments[1]->as.literal.raw_value.string_val);
        } else {
            snprintf(loc_buf, sizeof(loc_buf), "line %d", node->arguments[0]->line);
        }
        XrString *loc_str = xr_compile_time_intern(ctx->X, loc_buf, strlen(loc_buf));
        int loc_idx = xr_vm_proto_add_constant(compiler->proto, xr_string_value(loc_str));
        xemit_assert(compiler->emitter, cond_reg, loc_idx, 0);
        reg_free(compiler, cond_reg);
        int result_reg = reg_alloc(ctx, compiler);
        xemit_loadnull(compiler->emitter, result_reg);
        return result_reg;
    }
    xr_compiler_error(ctx, compiler, "assert() expects 1 or 2 arguments, got %d", node->arg_count);
    int r = reg_alloc(ctx, compiler); xemit_loadnull(compiler->emitter, r); return r;
}

static int compile_builtin_assert_eq(XrCompilerContext *ctx, XrCompiler *compiler, CallExprNode *node) {
    XR_DCHECK(ctx != NULL, "compile_builtin_assert_eq: NULL ctx");
    XR_DCHECK(compiler != NULL, "compile_builtin_assert_eq: NULL compiler");
    if (node->arg_count == 2) {
        // Use readonly to preserve raw type (no auto-boxing)
        XrExprDesc actual_desc = xr_compile_expr(ctx, compiler, node->arguments[0]);
        int actual_reg = xexpr_to_anyreg_readonly(ctx, compiler, &actual_desc);

        XrExprDesc expect_desc = xr_compile_expr(ctx, compiler, node->arguments[1]);
        int expect_reg = xexpr_to_anyreg_readonly(ctx, compiler, &expect_desc);

        char loc_buf[64];
        snprintf(loc_buf, sizeof(loc_buf), "line %d", node->arguments[0]->line);
        XrString *loc_str = xr_compile_time_intern(ctx->X, loc_buf, strlen(loc_buf));
        int loc_idx = xr_vm_proto_add_constant(compiler->proto, xr_string_value(loc_str));

        actual_reg = xexpr_ensure_boxed(ctx, compiler, &actual_desc, actual_reg);
        expect_reg = xexpr_ensure_boxed(ctx, compiler, &expect_desc, expect_reg);
        xemit_assert_eq(compiler->emitter, actual_reg, expect_reg, loc_idx);

        reg_free(compiler, expect_reg);
        reg_free(compiler, actual_reg);
        int result_reg = reg_alloc(ctx, compiler);
        xemit_loadnull(compiler->emitter, result_reg);
        return result_reg;
    }
    xr_compiler_error(ctx, compiler, "assert_eq() expects 2 arguments, got %d", node->arg_count);
    int r = reg_alloc(ctx, compiler); xemit_loadnull(compiler->emitter, r); return r;
}

static int compile_builtin_assert_false(XrCompilerContext *ctx, XrCompiler *compiler, CallExprNode *node) {
    if (node->arg_count == 1) {
        XrExprDesc cond_desc = xr_compile_expr(ctx, compiler, node->arguments[0]);
        int cond_reg = xexpr_to_anyreg(ctx, compiler, &cond_desc);
        char loc_buf[64];
        snprintf(loc_buf, sizeof(loc_buf), "line %d", node->arguments[0]->line);
        XrString *loc_str = xr_compile_time_intern(ctx->X, loc_buf, strlen(loc_buf));
        int loc_idx = xr_vm_proto_add_constant(compiler->proto, xr_string_value(loc_str));
        xemit_assert(compiler->emitter, cond_reg, loc_idx, 1);
        reg_free(compiler, cond_reg);
        int result_reg = reg_alloc(ctx, compiler);
        xemit_loadnull(compiler->emitter, result_reg);
        return result_reg;
    }
    xr_compiler_error(ctx, compiler, "assert_false() expects 1 argument, got %d", node->arg_count);
    int r = reg_alloc(ctx, compiler); xemit_loadnull(compiler->emitter, r); return r;
}

static int compile_builtin_assert_ne(XrCompilerContext *ctx, XrCompiler *compiler, CallExprNode *node) {
    if (node->arg_count == 2) {
        // Use readonly to preserve raw type (no auto-boxing)
        XrExprDesc actual_desc = xr_compile_expr(ctx, compiler, node->arguments[0]);
        int actual_reg = xexpr_to_anyreg_readonly(ctx, compiler, &actual_desc);

        XrExprDesc unexpected_desc = xr_compile_expr(ctx, compiler, node->arguments[1]);
        int unexpected_reg = xexpr_to_anyreg_readonly(ctx, compiler, &unexpected_desc);

        char loc_buf[64];
        snprintf(loc_buf, sizeof(loc_buf), "line %d", node->arguments[0]->line);
        XrString *loc_str = xr_compile_time_intern(ctx->X, loc_buf, strlen(loc_buf));
        int loc_idx = xr_vm_proto_add_constant(compiler->proto, xr_string_value(loc_str));

        actual_reg = xexpr_ensure_boxed(ctx, compiler, &actual_desc, actual_reg);
        unexpected_reg = xexpr_ensure_boxed(ctx, compiler, &unexpected_desc, unexpected_reg);
        xemit_assert_ne(compiler->emitter, actual_reg, unexpected_reg, loc_idx);

        reg_free(compiler, unexpected_reg);
        reg_free(compiler, actual_reg);
        int result_reg = reg_alloc(ctx, compiler);
        xemit_loadnull(compiler->emitter, result_reg);
        return result_reg;
    }
    xr_compiler_error(ctx, compiler, "assert_ne() expects 2 arguments, got %d", node->arg_count);
    int r = reg_alloc(ctx, compiler); xemit_loadnull(compiler->emitter, r); return r;
}

static int compile_builtin_assert_throws(XrCompilerContext *ctx, XrCompiler *compiler, CallExprNode *node) {
    if (node->arg_count == 1) {
        // Compile the closure argument
        XrExprDesc fn_desc = xr_compile_expr(ctx, compiler, node->arguments[0]);
        int fn_reg = xexpr_to_anyreg(ctx, compiler, &fn_desc);

        // Prepare location string for error message
        char loc_buf[64];
        snprintf(loc_buf, sizeof(loc_buf), "line %d", node->arguments[0]->line);
        XrString *loc_str = xr_compile_time_intern(ctx->X, loc_buf, strlen(loc_buf));
        int loc_idx = xr_vm_proto_add_constant(compiler->proto, xr_string_value(loc_str));

        // Use a flag register to track whether exception was thrown.
        // OP_ASSERT must be OUTSIDE the try-catch, otherwise its own
        // exception would be caught, making assert_throws always pass.
        //
        //   LOADTRUE flag_reg          (assume fn will throw)
        //   OP_TRY catch_offset
        //   OP_NOP 0
        //   OP_CALL fn_reg, 0, 1       (call closure)
        //   LOADFALSE flag_reg          (fn didn't throw → mark fail)
        //   JMP end
        // catch:
        //   OP_CATCH catch_reg
        // end:
        //   OP_END_TRY
        //   OP_ASSERT flag_reg, loc, 0  (check OUTSIDE try-catch)

        // 1. Flag register: true = expect throw, set to false if no throw
        int flag_reg = reg_alloc(ctx, compiler);
        xemit_loadtrue(compiler->emitter, flag_reg);

        // 2. OP_TRY (catch offset patched later)
        int try_pc = PROTO_CODE_COUNT(compiler->proto);
        emit_abx(compiler->emitter, OP_TRY, 0, 0);

        // 3. OP_NOP (no finally)
        xemit_nop(compiler->emitter, 0, 0, 0);

        // 4. Call the closure with 0 args
        xemit_call(compiler->emitter, fn_reg, 0, 1);

        // 5. No exception → set flag to false
        xemit_loadfalse(compiler->emitter, flag_reg);

        // 6. JMP to end (skip catch)
        int end_jump = emit_jump(compiler->emitter, OP_JMP);

        // 7. Catch block: absorb the exception
        int catch_start = PROTO_CODE_COUNT(compiler->proto);
        PROTO_SET_CODE(compiler->proto, try_pc, CREATE_ABx(OP_TRY, 0, catch_start));

        int catch_reg = reg_alloc(ctx, compiler);
        xemit_catch(compiler->emitter, catch_reg);
        reg_free(compiler, catch_reg);

        // 8. End: OP_END_TRY + assert flag OUTSIDE try-catch
        patch_jump(compiler->emitter, end_jump, -1);
        xemit_end_try(compiler->emitter);
        xemit_assert(compiler->emitter, flag_reg, loc_idx, 0);

        reg_free(compiler, flag_reg);
        reg_free(compiler, fn_reg);
        int result_reg = reg_alloc(ctx, compiler);
        xemit_loadnull(compiler->emitter, result_reg);
        return result_reg;
    }
    xr_compiler_error(ctx, compiler, "assert_throws() expects 1 argument (a function), got %d", node->arg_count);
    int r = reg_alloc(ctx, compiler); xemit_loadnull(compiler->emitter, r); return r;
}

static int compile_builtin_assert_true(XrCompilerContext *ctx, XrCompiler *compiler, CallExprNode *node) {
    if (node->arg_count == 1) {
        XrExprDesc cond_desc = xr_compile_expr(ctx, compiler, node->arguments[0]);
        int cond_reg = xexpr_to_anyreg(ctx, compiler, &cond_desc);
        char loc_buf[64];
        snprintf(loc_buf, sizeof(loc_buf), "line %d", node->arguments[0]->line);
        XrString *loc_str = xr_compile_time_intern(ctx->X, loc_buf, strlen(loc_buf));
        int loc_idx = xr_vm_proto_add_constant(compiler->proto, xr_string_value(loc_str));
        xemit_assert(compiler->emitter, cond_reg, loc_idx, 0);
        reg_free(compiler, cond_reg);
        int result_reg = reg_alloc(ctx, compiler);
        xemit_loadnull(compiler->emitter, result_reg);
        return result_reg;
    }
    xr_compiler_error(ctx, compiler, "assert_true() expects 1 argument, got %d", node->arg_count);
    int r = reg_alloc(ctx, compiler); xemit_loadnull(compiler->emitter, r); return r;
}

static int compile_builtin_array(XrCompilerContext *ctx, XrCompiler *compiler, CallExprNode *node) {
    int result_reg = reg_alloc(ctx, compiler);
    int c_field = ((int)ctx->current_elem_tid << 2) | ctx->current_storage_mode;
    if (node->arg_count == 0) {
        xemit_newarray(compiler->emitter, result_reg, 0, c_field);
    } else if (node->arg_count == 1) {
        XrExprDesc arg_desc = xr_compile_expr(ctx, compiler, node->arguments[0]);
        int arg_reg = xexpr_to_anyreg(ctx, compiler, &arg_desc);
        xemit_newarray(compiler->emitter, result_reg, arg_reg, c_field);
        xreg_set_freereg(compiler->regalloc, result_reg + 1);
    } else {
        xr_compiler_error(ctx, compiler, "Array() expects 0 or 1 argument, got %d", node->arg_count);
        xemit_newarray(compiler->emitter, result_reg, 0, c_field);
    }
    return result_reg;
}

static int compile_builtin_bool(XrCompilerContext *ctx, XrCompiler *compiler, CallExprNode *node) {
    if (node->arg_count == 1) return compile_builtin_func(ctx, compiler, node->arguments[0], OP_TOBOOL);
    return BUILTIN_NOT_HANDLED;
}

static int compile_builtin_bytes(XrCompilerContext *ctx, XrCompiler *compiler, CallExprNode *node) {
    int arg_regs[2] = {-1, -1};
    for (int i = 0; i < node->arg_count && i < 2; i++) {
        XrExprDesc arg_desc = xr_compile_expr(ctx, compiler, node->arguments[i]);
        arg_regs[i] = xexpr_to_anyreg(ctx, compiler, &arg_desc);
    }
    int base_reg = reg_alloc(ctx, compiler);
    for (int i = 0; i < node->arg_count && i < 2; i++) {
        int target_reg = reg_alloc(ctx, compiler);
        if (arg_regs[i] != target_reg) {
            xemit_move(compiler->emitter, target_reg, arg_regs[i]);
            reg_free(compiler, arg_regs[i]);
        }
    }
    xemit_bytes_new(compiler->emitter, base_reg, node->arg_count);
    for (int i = node->arg_count - 1; i >= 0; i--) {
        reg_free(compiler, base_reg + 1 + i);
    }
    return base_reg;
}

static int compile_builtin_chr(XrCompilerContext *ctx, XrCompiler *compiler, CallExprNode *node) {
    if (node->arg_count == 1) return compile_builtin_func(ctx, compiler, node->arguments[0], OP_CHR);
    return BUILTIN_NOT_HANDLED;
}

static int compile_builtin_copy(XrCompilerContext *ctx, XrCompiler *compiler, CallExprNode *node) {
    if (node->arg_count == 1) {
        XrExprDesc arg_desc = xr_compile_expr(ctx, compiler, node->arguments[0]);
        int arg_reg = xexpr_to_anyreg(ctx, compiler, &arg_desc);
        int result_reg = reg_alloc(ctx, compiler);
        xemit_copy(compiler->emitter, result_reg, arg_reg);
        xreg_set_freereg(compiler->regalloc, result_reg + 1);
        return result_reg;
    }
    xr_compiler_error(ctx, compiler, "copy() expects 1 argument, got %d", node->arg_count);
    int r = reg_alloc(ctx, compiler); xemit_loadnull(compiler->emitter, r); return r;
}

static int compile_builtin_dump(XrCompilerContext *ctx, XrCompiler *compiler, CallExprNode *node) {
    // OP_DUMP contract: A = value register, B = literal indent (0..8), C = unused.
    // The indent argument must be a compile-time integer literal so it can be
    // encoded directly in the B field (the VM/JIT read it as a raw int, not a
    // register reference).
    if (node->arg_count == 1 || node->arg_count == 2) {
        XrExprDesc val_desc = xr_compile_expr(ctx, compiler, node->arguments[0]);
        int val_reg = xexpr_to_anyreg(ctx, compiler, &val_desc);

        int indent = 0;
        if (node->arg_count == 2) {
            AstNode *arg1 = node->arguments[1];
            if (arg1->type != AST_LITERAL_INT) {
                xr_compiler_error(ctx, compiler,
                    "dump() indent argument must be an integer literal");
            } else {
                int64_t v = arg1->as.literal.raw_value.int_val;
                if (v < 0) v = 0;
                if (v > 8) v = 8;
                indent = (int)v;
            }
        }
        xemit_dump(compiler->emitter, val_reg, indent);
        reg_free(compiler, val_reg);

        // dump() returns null
        int result_reg = reg_alloc(ctx, compiler);
        xemit_loadnull(compiler->emitter, result_reg);
        return result_reg;
    }
    xr_compiler_error(ctx, compiler, "dump() expects 1 or 2 arguments, got %d", node->arg_count);
    int r = reg_alloc(ctx, compiler); xemit_loadnull(compiler->emitter, r); return r;
}

static int compile_builtin_float(XrCompilerContext *ctx, XrCompiler *compiler, CallExprNode *node) {
    if (node->arg_count == 1) return compile_builtin_func(ctx, compiler, node->arguments[0], OP_TOFLOAT);
    return BUILTIN_NOT_HANDLED;
}

static int compile_builtin_int(XrCompilerContext *ctx, XrCompiler *compiler, CallExprNode *node) {
    if (node->arg_count == 1) return compile_builtin_func(ctx, compiler, node->arguments[0], OP_TOINT);
    return BUILTIN_NOT_HANDLED;
}

static int compile_builtin_map(XrCompilerContext *ctx, XrCompiler *compiler, CallExprNode *node) {
    (void)node;
    int result_reg = reg_alloc(ctx, compiler);
    int key_kind = (ctx->current_key_tid == XR_TID_STRING) ? 1 : (ctx->current_key_tid == XR_TID_INT) ? 2 : 0;
    int c_field = (key_kind << 7) | (((int)ctx->current_elem_tid & 0x1F) << 2) | ctx->current_storage_mode;
    xemit_newmap(compiler->emitter, result_reg, 0, c_field);
    return result_reg;
}

static int compile_builtin_set(XrCompilerContext *ctx, XrCompiler *compiler, CallExprNode *node) {
    int result_reg = reg_alloc(ctx, compiler);
    int b_field = ((int)ctx->current_elem_tid << 2) | ctx->current_storage_mode;
    if (node->arg_count == 0) {
        xemit_newset(compiler->emitter, result_reg, b_field);
    } else if (node->arg_count == 1) {
        int arg_reg = result_reg + 1;
        xreg_set_freereg(compiler->regalloc, arg_reg);
        XrExprDesc arg_desc = xr_compile_expr(ctx, compiler, node->arguments[0]);
        xexpr_to_specific_reg(ctx, compiler, &arg_desc, arg_reg);
        xemit_newset(compiler->emitter, result_reg, b_field);
        xreg_set_freereg(compiler->regalloc, result_reg + 1);
    } else {
        xr_compiler_error(ctx, compiler, "Set() expects 0 or 1 argument (array)");
        xemit_newset(compiler->emitter, result_reg, b_field);
    }
    return result_reg;
}

static int compile_builtin_string(XrCompilerContext *ctx, XrCompiler *compiler, CallExprNode *node) {
    if (node->arg_count == 1) {
        XrExprDesc arg_desc = xr_compile_expr(ctx, compiler, node->arguments[0]);
        // Preserve raw format — slot_hint tells VM to format directly
        int slot_hint = 0;
        if (xexpr_is_raw_i64(&arg_desc))      slot_hint = 1;
        else if (xexpr_is_raw_f64(&arg_desc)) slot_hint = 2;
        int arg_reg = (slot_hint != 0)
            ? xexpr_to_anyreg_readonly(ctx, compiler, &arg_desc)
            : xexpr_to_anyreg(ctx, compiler, &arg_desc);
        int result_reg = reg_alloc(ctx, compiler);
        xemit_tostring(compiler->emitter, result_reg, arg_reg, slot_hint);
        xreg_set_freereg(compiler->regalloc, result_reg + 1);
        return result_reg;
    }
    return BUILTIN_NOT_HANDLED;
}

// Helper: compile typeof/typename with given opcode
static int compile_typeof_impl(XrCompilerContext *ctx, XrCompiler *compiler, CallExprNode *node, int opcode, const char *func_name) {
    XR_DCHECK(ctx != NULL, "compile_typeof_impl: NULL ctx");
    XR_DCHECK(compiler != NULL, "compile_typeof_impl: NULL compiler");
    if (node->arg_count == 1) {
        XrExprDesc arg_desc = xr_compile_expr(ctx, compiler, node->arguments[0]);
        int slot_hint = 0;
        if (xexpr_is_raw_i64(&arg_desc))      slot_hint = 1;
        else if (xexpr_is_raw_f64(&arg_desc)) slot_hint = 2;
        int arg_reg = (slot_hint != 0)
            ? xexpr_to_anyreg_readonly(ctx, compiler, &arg_desc)
            : xexpr_to_anyreg(ctx, compiler, &arg_desc);
        int result_reg = reg_alloc(ctx, compiler);
        emit_abc(compiler->emitter, opcode, result_reg, arg_reg, slot_hint);
        xreg_set_freereg(compiler->regalloc, result_reg + 1);
        return result_reg;
    }
    xr_compiler_error(ctx, compiler, "%s() expects 1 argument, got %d", func_name, node->arg_count);
    int r = reg_alloc(ctx, compiler); xemit_loadnull(compiler->emitter, r); return r;
}

static int compile_builtin_typeof(XrCompilerContext *ctx, XrCompiler *compiler, CallExprNode *node) {
    return compile_typeof_impl(ctx, compiler, node, OP_TYPEOF, "typeof");
}

static int compile_builtin_typename(XrCompilerContext *ctx, XrCompiler *compiler, CallExprNode *node) {
    return compile_typeof_impl(ctx, compiler, node, OP_TYPENAME, "typename");
}

static int compile_builtin_weakmap(XrCompilerContext *ctx, XrCompiler *compiler, CallExprNode *node) {
    if (node->arg_count == 0) {
        int result_reg = reg_alloc(ctx, compiler);
        int key_kind = (ctx->current_key_tid == XR_TID_STRING) ? 1 : (ctx->current_key_tid == XR_TID_INT) ? 2 : 0;
        int c_field = (key_kind << 7) | (((int)ctx->current_elem_tid & 0x1F) << 2) | 0x02;
        xemit_newmap(compiler->emitter, result_reg, 0, c_field);
        return result_reg;
    }
    return BUILTIN_NOT_HANDLED;
}

static int compile_builtin_weakset(XrCompilerContext *ctx, XrCompiler *compiler, CallExprNode *node) {
    if (node->arg_count == 0) {
        int result_reg = reg_alloc(ctx, compiler);
        int b_field = ((int)ctx->current_elem_tid << 2) | 0x02;
        xemit_newset(compiler->emitter, result_reg, b_field);
        return result_reg;
    }
    return BUILTIN_NOT_HANDLED;
}

// Sorted by name for binary search (21 entries)
static const BuiltinEntry builtin_functions[] = {
    {"Array",        compile_builtin_array},
    {"Bytes",        compile_builtin_bytes},
    {"Map",          compile_builtin_map},
    {"Set",          compile_builtin_set},
    {"WeakMap",      compile_builtin_weakmap},
    {"WeakSet",      compile_builtin_weakset},
    {"assert",       compile_builtin_assert},
    {"assert_eq",    compile_builtin_assert_eq},
    {"assert_false", compile_builtin_assert_false},
    {"assert_ne",      compile_builtin_assert_ne},
    {"assert_throws",  compile_builtin_assert_throws},
    {"assert_true",    compile_builtin_assert_true},
    {"bool",         compile_builtin_bool},
    {"chr",          compile_builtin_chr},
    {"copy",         compile_builtin_copy},
    {"dump",         compile_builtin_dump},
    {"float",        compile_builtin_float},
    {"int",          compile_builtin_int},
    {"string",       compile_builtin_string},
    {"typename",     compile_builtin_typename},
    {"typeof",       compile_builtin_typeof},
};
#define BUILTIN_COUNT (sizeof(builtin_functions) / sizeof(builtin_functions[0]))

static const BuiltinEntry *builtin_lookup(const char *name) {
    int lo = 0, hi = BUILTIN_COUNT - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int cmp = strcmp(name, builtin_functions[mid].name);
        if (cmp < 0) hi = mid - 1;
        else if (cmp > 0) lo = mid + 1;
        else return &builtin_functions[mid];
    }
    return NULL;
}

/* ========== Public dispatcher ========== */

int xr_compile_call_builtin(XrCompilerContext *ctx, XrCompiler *compiler,
                            CallExprNode *node, bool is_tail) {
    XR_DCHECK(ctx != NULL, "xr_compile_call_builtin: NULL ctx");
    XR_DCHECK(compiler != NULL, "xr_compile_call_builtin: NULL compiler");
    XR_DCHECK(node != NULL, "xr_compile_call_builtin: NULL node");

    // Builtins are recognised by call-site name only: foo(...).
    // `obj.method(...)` and other shapes are not in this table.
    if (node->callee->type != AST_VARIABLE) return -1;

    const BuiltinEntry *entry = builtin_lookup(node->callee->as.variable.name);
    if (!entry) return -1;

    int result = entry->compile(ctx, compiler, node);
    if (result == BUILTIN_NOT_HANDLED) return -1;

    // Builtins emit specialised opcodes (e.g. OP_TOINT), not OP_CALL /
    // OP_TAILCALL. If called in tail position, the caller assumes
    // TAILCALL has implicit return semantics, so we must emit RETURN1
    // explicitly here.
    if (is_tail) {
        emit_return(compiler->emitter, result, 1);
        reg_free(compiler, result);
    }
    return result;
}
