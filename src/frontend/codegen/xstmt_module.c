/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xstmt_module.c - Xray module system statement compilation
 *
 * KEY CONCEPT:
 *   - import http              - standard library
 *   - import xray/redis        - third-party package
 *   - import "./utils"         - local module
 *   - export fn add() {}
 *   - export let PI = 3.14
 *
 * RELATED MODULES:
 *   - xmodule.c: Runtime module system
 *   - export class User {}
 */

#include "xstmt.h"
#include "../../base/xchecks.h"
#include "../../base/xlog.h"
#include "xcompiler.h"
#include "xcompiler_context.h"
#include "xemit.h"
#include "xexpr.h"
#include "xexpr_desc.h"
#include "../../runtime/xisolate_api.h"
#include <stdio.h>
#include <string.h>
#include "../../runtime/symbol/xsymbol_table.h"

// ========== Import Statement ==========

/*
 * Compile import statement
 *
 * import time
 * import time as t
 *
 * Implementation strategy:
 * Always store imported module as local variable, functions in module accessed via upvalue
 * This makes each module's imports independent, no conflicts with other modules
 *
 * @param ctx Compiler context
 * @param c   Compiler
 * @param node Import statement node
 */
void compile_import(XrCompilerContext *ctx, XrCompiler *c, ImportStmtNode *node) {
    XR_DCHECK(ctx != NULL, "compile_import: NULL ctx");
    XR_DCHECK(c != NULL, "compile_import: NULL compiler");
    XR_DCHECK(node != NULL, "compile_import: NULL node");
    // V2 simplified: use module name directly, no prefix

    // Load module name to constant pool
    XrString *module_name =
        xr_compile_time_intern(ctx->X, node->module_name, strlen(node->module_name));
    XrValue module_name_val = xr_string_value(module_name);
    int name_idx = xr_vm_proto_add_constant(c->proto, module_name_val);

    // Allocate module register
    int module_reg = reg_alloc(ctx, c);

    // Generate OP_IMPORT instruction: R[A] = import(K[Bx])
    xemit_import(c->emitter, module_reg, name_idx);

    // Named import: import { a, b } from "module"
    if (node->member_count > 0) {
        for (int i = 0; i < node->member_count; i++) {
            ImportMember *member = &node->members[i];
            const char *local_name = member->alias ? member->alias : member->name;

            // Check if local variable with same name already exists
            XrLocalInfo *existing = compiler_get_local_by_name(c, local_name);
            if (existing && existing->depth == c->scope_depth) {
                xr_compiler_error(ctx, c, "import member '%s' conflicts with existing variable",
                                  local_name);
                reg_free(c, module_reg);
                return;
            }

            // Allocate member register
            int member_reg = reg_alloc(ctx, c);

            // Get symbol ID for member name (local index via per-function symbol table)
            int global_sym =
                xr_symbol_register_in_table(xr_isolate_get_symbol_table(ctx->X), member->name);
            int local_sym = emitter_add_symbol(c->emitter, global_sym);

            // Generate OP_GETPROP instruction: R[member_reg] = R[module_reg].symbol[local_sym]
            xemit_getprop(c->emitter, member_reg, module_reg, local_sym);

            // Define as local variable
            XrString *local_str = xr_compile_time_intern(ctx->X, local_name, strlen(local_name));
            scope_define_local_reg(ctx, c, local_str, member_reg);
        }

        // Release module register after named import (module object itself not needed)
        reg_free(c, module_reg);
    } else {
        // Whole import: import "module" as name
        const char *var_name = node->alias ? node->alias : node->module_name;
        XrString *var_name_str = xr_compile_time_intern(ctx->X, var_name, strlen(var_name));

        // Check if shared variable with same name exists (consider lexical scope)
        int existing_shared = shared_get_in_scope(ctx, c, var_name_str);
        if (existing_shared >= 0) {
            xr_compiler_error(ctx, c,
                              "module variable '%s' already defined, use 'import ... as alias' to "
                              "specify different name",
                              var_name);
            reg_free(c, module_reg);
            return;
        }

        // Store module in shared_array (global heap, coroutine-safe, not via upvalue)
        int shared_index = shared_get_or_add(ctx, c, var_name_str);
        shared_set_const(ctx, shared_index, true);
        xemit_setshared(c->emitter, module_reg, shared_index);
        reg_free(c, module_reg);
    }
}

// ========== Export Statement ==========

/*
 * Compile export statement
 *
 * export fn add() {}
 * export let PI = 3.14
 *
 * Implementation strategy:
 * 1. Compile the exported declaration (function/variable)
 * 2. Get the export name
 * 3. Get export value (from global or local variable)
 * 4. Generate OP_EXPORT instruction
 *
 * @param ctx Compiler context
 * @param c   Compiler
 * @param node Export statement node
 */
void compile_export(XrCompilerContext *ctx, XrCompiler *c, ExportStmtNode *node) {
    XR_DCHECK(ctx != NULL, "compile_export: NULL ctx");
    XR_DCHECK(c != NULL, "compile_export: NULL compiler");
    XR_DCHECK(node != NULL, "compile_export: NULL node");
    // export can only be used at top-level scope
    if (c->scope_depth > 0 || c->type != FUNCTION_SCRIPT) {
        xr_compiler_error(
            ctx, c, "export can only be used at module top-level, not inside function or block");
        return;
    }

    // Check if re-export (export { ... } from "...")
    if (node->from_path) {
        // 1. Import source module
        XrString *module_name =
            xr_compile_time_intern(ctx->X, node->from_path, strlen(node->from_path));
        XrValue module_name_val = xr_string_value(module_name);
        int name_idx = xr_vm_proto_add_constant(c->proto, module_name_val);

        int module_reg = reg_alloc(ctx, c);
        xemit_import(c->emitter, module_reg, name_idx);

        if (node->is_reexport_all) {
            // export * from "..." - export all members from module
            // Generate OP_EXPORT_ALL instruction: copy all exports from module to current module
            xemit_export_all(c->emitter, module_reg);
        } else {
            // export { a, b as c } from "..." - export specific members
            for (int i = 0; i < node->reexport_count; i++) {
                ReexportMember *member = &node->reexport_members[i];
                const char *src_name = member->name;
                const char *dst_name = member->alias ? member->alias : member->name;

                // Get member from source module
                int member_reg = reg_alloc(ctx, c);
                int global_sym =
                    xr_symbol_register_in_table(xr_isolate_get_symbol_table(ctx->X), src_name);
                int local_sym = emitter_add_symbol(c->emitter, global_sym);
                xemit_getprop(c->emitter, member_reg, module_reg, local_sym);

                // Export to current module
                XrString *dst_str = xr_compile_time_intern(ctx->X, dst_name, strlen(dst_name));
                XrValue dst_val = xr_string_value(dst_str);
                int dst_idx = xr_vm_proto_add_constant(c->proto, dst_val);
                xemit_export(c->emitter, dst_idx, member_reg, 0);

                reg_free(c, member_reg);
            }
        }

        reg_free(c, module_reg);
        return;
    }

    // Check if list-style export (export a, b, c)
    if (node->export_names && node->export_count > 0) {
        // List-style export: iterate each variable name
        for (int i = 0; i < node->export_count; i++) {
            const char *name = node->export_names[i];
            XrString *name_str = xr_compile_time_intern(ctx->X, name, strlen(name));
            int value_reg = reg_alloc(ctx, c);
            int is_const = 0;

            // Lookup local variable info (contains is_const flag)
            XrLocalInfo *local_info = compiler_get_local_by_name(c, name);

            if (local_info) {
                // Found local variable, get is_const info
                is_const = local_info->is_const ? 1 : 0;
                xemit_move(c->emitter, value_reg, local_info->reg);
            } else if (c->scope_depth == 0) {
                // Top-level scope: check shared first, then predefined globals
                int si = shared_get_in_scope(ctx, c, name_str);
                if (si >= 0) {
                    xemit_getshared(c->emitter, value_reg, si);
                    is_const = shared_is_const(ctx, si) ? 1 : 0;
                } else {
                    int gi = builtin_get(ctx, name_str);
                    if (gi >= 0) {
                        xemit_getbuiltin(c->emitter, value_reg, gi);
                    }
                }
            } else {
                xr_log_warning("compiler", "export: cannot find variable '%s'", name);
                reg_free(c, value_reg);
                continue;
            }

            // Generate OP_EXPORT instruction
            XrValue name_val = xr_string_value(name_str);
            int name_idx = xr_vm_proto_add_constant(c->proto, name_val);
            xemit_export(c->emitter, name_idx, value_reg, is_const);

            reg_free(c, value_reg);
        }
        return;
    }

    // Declaration-style export (export let/const/fn/class ...)

    // 1. Compile the exported declaration
    xr_compile_statement(ctx, c, node->declaration);

    // 2. Get export name
    const char *export_name = node->export_name;
    if (!export_name && node->declaration) {
        // Extract name from declaration
        AstNode *decl = node->declaration;
        if (decl->type == AST_FUNCTION_DECL) {
            export_name = decl->as.function_decl.name;
        } else if (decl->type == AST_VAR_DECL) {
            export_name = decl->as.var_decl.name;
        } else if (decl->type == AST_CONST_DECL) {
            export_name = decl->as.var_decl.name;
        } else if (decl->type == AST_CLASS_DECL) {
            export_name = decl->as.class_decl.name;
        } else if (decl->type == AST_STRUCT_DECL) {
            export_name = decl->as.struct_decl.name;
        }
    }

    if (!export_name) {
        return;
    }

    // 3. Get export value
    XrString *name_str = xr_compile_time_intern(ctx->X, export_name, strlen(export_name));
    int value_reg = reg_alloc(ctx, c);

    // Lookup order: local variable -> shared variable -> global variable
    int local_slot = scope_resolve_local(c, name_str);
    int shared_idx = shared_get_in_scope(ctx, c, name_str);

    if (local_slot >= 0) {
        // Local variable (non-pure function, regular variables, etc)
        xemit_move(c->emitter, value_reg, local_slot);
    } else if (shared_idx >= 0) {
        // Shared variable (pure function, imported module, etc)
        xemit_getshared(c->emitter, value_reg, shared_idx);
    } else if (c->scope_depth == 0) {
        // Predefined globals fallback (user variables already found via shared above)
        int gi = builtin_get(ctx, name_str);
        if (gi >= 0) {
            xemit_getbuiltin(c->emitter, value_reg, gi);
        }
    } else {
        xr_log_warning("compiler", "export: cannot find variable '%s'", export_name);
    }

    // 4. Generate OP_EXPORT instruction (C param: 0=let variable, 1=const constant)
    int is_const = 0;
    if (node->declaration && node->declaration->type == AST_CONST_DECL) {
        is_const = 1;
    }

    XrValue name_val = xr_string_value(name_str);
    int name_idx = xr_vm_proto_add_constant(c->proto, name_val);
    xemit_export(c->emitter, name_idx, value_reg, is_const);

    reg_free(c, value_reg);
}

// export default removed, default export syntax no longer supported
