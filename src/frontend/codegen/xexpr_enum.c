/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xexpr_enum.c - Xray enum expression compilation
 *
 * KEY CONCEPT:
 *   - Enum access (Enum.Member)
 *   - Enum conversion (Enum(value))
 */

#include "xexpr.h"
#include "../../base/xchecks.h"
#include "xcompiler.h"
#include "xcompiler_context.h"
#include "xemit.h"
#include "xexpr_desc.h"
#include <stdio.h>
#include <string.h>

// External functions
// - xr_compiler_ctx_find_global (needs to be public)

// ========== Enum Access ==========

/*
 * Internal implementation: compile enum access (returns register)
 */
static int compile_enum_access_internal(XrCompilerContext *ctx, XrCompiler *compiler,
                                        EnumAccessNode *node) {
    XR_DCHECK(ctx != NULL, "compile_enum_access: NULL ctx");
    XR_DCHECK(compiler != NULL, "compile_enum_access: NULL compiler");
    XR_DCHECK(node != NULL, "compile_enum_access: NULL node");
    // Lookup enum type (check shared first, then globals)
    XrString *enum_name_str =
        xr_compile_time_intern(ctx->X, node->enum_name, strlen(node->enum_name));
    int enum_shared_idx = shared_get_in_scope(ctx, compiler, enum_name_str);
    int enum_global_idx = -1;
    if (enum_shared_idx < 0) {
        enum_global_idx = builtin_get(ctx, enum_name_str);
    }

    if (enum_shared_idx == -1 && enum_global_idx == -1) {
        xr_compiler_error(ctx, compiler, "undefined enum type: %s", node->enum_name);
        return reg_alloc(ctx, compiler);
    }

    // Use compile-time interning to ensure constant pool strings are comparable
    XrString *member_name_str =
        xr_compile_time_intern(ctx->X, node->member_name, strlen(node->member_name));
    XrValue member_name_val = xr_string_value(member_name_str);
    int member_const_idx = xr_vm_proto_add_constant(compiler->proto, member_name_val);

    // Allocate result register
    int result_reg = reg_alloc(ctx, compiler);

    // Load enum type object
    int enum_reg = reg_alloc(ctx, compiler);
    if (enum_shared_idx >= 0) {
        xemit_getshared(compiler->emitter, enum_reg, enum_shared_idx);
    } else {
        xemit_getbuiltin(compiler->emitter, enum_reg, enum_global_idx);
    }

    // Access member
    xemit_map_getk(compiler->emitter, result_reg, enum_reg, member_const_idx);

    // Set freereg = result_reg + 1, reclaim enum_reg
    xreg_set_freereg(compiler->regalloc, result_reg + 1);

    return result_reg;
}

XrExprDesc compile_enum_access(XrCompilerContext *ctx, XrCompiler *compiler, EnumAccessNode *node) {
    XrExprDesc e = {0};
    int reg = compile_enum_access_internal(ctx, compiler, node);
    xexpr_init(&e, XEXPR_TEMP, reg);
    return e;
}

// ========== Enum Conversion ==========

/*
 * Internal implementation: compile enum conversion (returns register)
 */
static int compile_enum_convert_internal(XrCompilerContext *ctx, XrCompiler *compiler,
                                         EnumConvertNode *node) {
    // Lookup enum type (check shared first, then globals)
    XrString *enum_name_str =
        xr_compile_time_intern(ctx->X, node->enum_name, strlen(node->enum_name));
    int enum_shared_idx = shared_get_in_scope(ctx, compiler, enum_name_str);
    int enum_global_idx = -1;
    if (enum_shared_idx < 0) {
        enum_global_idx = builtin_get(ctx, enum_name_str);
    }

    if (enum_shared_idx == -1 && enum_global_idx == -1) {
        xr_compiler_error(ctx, compiler, "undefined enum type: %s", node->enum_name);
        return reg_alloc(ctx, compiler);
    }

    // Load enum type object
    int enum_reg = reg_alloc(ctx, compiler);
    if (enum_shared_idx >= 0) {
        xemit_getshared(compiler->emitter, enum_reg, enum_shared_idx);
    } else {
        xemit_getbuiltin(compiler->emitter, enum_reg, enum_global_idx);
    }

    // Compile value expression
    XrExprDesc value_expr = xr_compile_expr(ctx, compiler, node->value_expr);
    int value_reg = xexpr_to_anyreg(ctx, compiler, &value_expr);

    // Allocate result register
    int result_reg = reg_alloc(ctx, compiler);

    // Generate ENUM_CONVERT instruction
    xemit_enum_convert(compiler->emitter, result_reg, enum_reg, value_reg);

    // Free temporary registers
    reg_free(compiler, value_reg);
    reg_free(compiler, enum_reg);

    return result_reg;
}

XrExprDesc compile_enum_convert(XrCompilerContext *ctx, XrCompiler *compiler,
                                EnumConvertNode *node) {
    XrExprDesc e = {0};
    int reg = compile_enum_convert_internal(ctx, compiler, node);
    xexpr_init(&e, XEXPR_TEMP, reg);
    return e;
}

// ========== Enum Index (for-in desugaring) ==========

XrExprDesc compile_enum_index(XrCompilerContext *ctx, XrCompiler *compiler, EnumIndexNode *node) {
    // Compile enum type expression (e.g. Color)
    XrExprDesc enum_expr = xr_compile_expr(ctx, compiler, node->collection);
    int enum_reg = xexpr_to_anyreg(ctx, compiler, &enum_expr);

    // Compile index expression (e.g. __for_idx)
    XrExprDesc idx_expr = xr_compile_expr(ctx, compiler, node->index_expr);
    int idx_reg = xexpr_to_anyreg(ctx, compiler, &idx_expr);

    // Allocate result register
    int result_reg = reg_alloc(ctx, compiler);

    // Emit OP_ENUM_ACCESS: R(a) = enum_type.members[R(c)]
    xemit_enum_access(compiler->emitter, result_reg, enum_reg, idx_reg);

    // Free temporary registers
    reg_free(compiler, idx_reg);
    reg_free(compiler, enum_reg);

    XrExprDesc e = {0};
    xexpr_init(&e, XEXPR_TEMP, result_reg);
    return e;
}
