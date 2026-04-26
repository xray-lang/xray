/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xexpr_object.c - Object expression compilation
 *
 * KEY CONCEPT:
 *   Handles object-related expressions:
 *   - new expression (object construction)
 *   - member access (obj.member)
 */

#include "xexpr.h"
#include "../../base/xchecks.h"
#include "../../runtime/xisolate_api.h"  // XrayIsolate full definition
#include "xcompiler.h"
#include "xexpr_desc.h"
#include "xcompiler_context.h"
#include "xcompiler_class_registry.h"  // Compile-time optimization
#include "xemit.h"
#include "xregalloc.h"
#include "../parser/xast.h"  // AST node definitions
#include "../../runtime/value/xtype.h"
#include "../../runtime/class/xclass.h"  // XrClass definition
#include "../../runtime/class/xclass_descriptor.h"  // XrClassDescriptor
#include "../../runtime/value/xstruct_layout.h"  // XrStructLayout for native struct storage
#include <string.h>  // strcmp
#include "../../runtime/symbol/xsymbol_table.h"  // Unified symbol table system
#include <stdio.h>
#include <string.h>

// New symbol system doesn't need global variables, xr_symbol_register/get_name are global functions

// ========== new Expression ==========

/*
 * Load class object to specified register
 * Supports two forms:
 *   - Plain class name: load from global variable
 *   - module.Class: load from module property
 */
static void load_class_to_reg(XrCompilerContext *ctx, XrCompiler *compiler,
                              const char *module_name, const char *class_name, int target_reg) {
    XR_DCHECK(ctx != NULL, "load_class_to_reg: NULL ctx");
    XR_DCHECK(compiler != NULL, "load_class_to_reg: NULL compiler");
    XR_DCHECK(class_name != NULL, "load_class_to_reg: NULL class_name");
    if (module_name != NULL) {
        // new module.Class() form: first load module, then get property
        XrString *mod_name_str = xr_compile_time_intern(ctx->X, module_name, strlen(module_name));

        // Find module variable (local, shared, or predefined global)
        int module_reg = scope_resolve_local(compiler, mod_name_str);
        if (module_reg < 0) {
            int si = shared_get_in_scope(ctx, compiler, mod_name_str);
            if (si >= 0) {
                xemit_getshared(compiler->emitter, target_reg, si);
            } else {
                int gi = builtin_get(ctx, mod_name_str);
                if (gi >= 0) {
                    xemit_getbuiltin(compiler->emitter, target_reg, gi);
                }
            }
            module_reg = target_reg;
        }

        // Get class from module (using OP_GETPROP)
        int global_sym = xr_symbol_register_in_table((XrSymbolTable*)xr_isolate_get_symbol_table(ctx->X), class_name);
        int local_sym = emitter_add_symbol(compiler->emitter, global_sym);
        xemit_getprop(compiler->emitter, target_reg, module_reg, local_sym);
    } else {
        // Plain class name: prefer local variable/upvalue, finally global variable
        XrString *class_name_str = xr_compile_time_intern(ctx->X, class_name, strlen(class_name));

        // 1. Find local variable
        XrLocalInfo *local = compiler_get_local_by_name(compiler, class_name);
        if (local) {
            xemit_move(compiler->emitter, target_reg, local->reg);
        } else {
            // 2. Find upvalue
            int upvalue = scope_resolve_upvalue(ctx, compiler, class_name_str);
            if (upvalue >= 0) {
                // Class names are always const, direct UPVAL_GET
                xemit_upval_get(compiler->emitter, target_reg, upvalue, 0);
            } else {
                // 3. Load from shared or predefined global
                int si = shared_get_in_scope(ctx, compiler, class_name_str);
                if (si >= 0) {
                    xemit_getshared(compiler->emitter, target_reg, si);
                } else {
                    int gi = builtin_get(ctx, class_name_str);
                    if (gi >= 0) {
                        xemit_getbuiltin(compiler->emitter, target_reg, gi);
                    }
                }
            }
        }
    }
}

/*
 * Internal implementation: compile new expression (returns register)
 * Supports two forms:
 *   new ClassName()           - Normal class instantiation
 *   new module.ClassName()    - Module class instantiation
 */
static int compile_new_expr_internal(XrCompilerContext *ctx, XrCompiler *compiler, NewExprNode *node) {
    // ========== Built-in type construction (supports new syntax) ==========
    if (node->module_name == NULL) {
        const char *class_name = node->class_name;

        // new Map() → OP_NEWMAP
        if (strcmp(class_name, TYPE_NAME_MAP) == 0 && node->arg_count == 0) {
            int result_reg = reg_alloc(ctx, compiler);
            int key_kind = (ctx->current_key_tid == XR_TID_STRING) ? 1 : (ctx->current_key_tid == XR_TID_INT) ? 2 : 0;
            int c_field = (key_kind << 7) | (((int)ctx->current_elem_tid & 0x1F) << 2) | ctx->current_storage_mode;
            xemit_newmap(compiler->emitter, result_reg, 0, c_field);
            return result_reg;
        }

        // new WeakMap() → OP_NEWMAP with weak flag (C bit1 = weak)
        if (strcmp(class_name, TYPE_NAME_WEAKMAP) == 0 && node->arg_count == 0) {
            int result_reg = reg_alloc(ctx, compiler);
            int key_kind = (ctx->current_key_tid == XR_TID_STRING) ? 1 : (ctx->current_key_tid == XR_TID_INT) ? 2 : 0;
            int c_field = (key_kind << 7) | (((int)ctx->current_elem_tid & 0x1F) << 2) | 0x02;
            xemit_newmap(compiler->emitter, result_reg, 0, c_field);
            return result_reg;
        }

        // new Array() or new Array(a, b, c) -> OP_NEWARRAY
        if (strcmp(class_name, TYPE_NAME_ARRAY) == 0) {
            int result_reg = reg_alloc(ctx, compiler);
            int c_field = ((int)ctx->current_elem_tid << 2) | ctx->current_storage_mode;
            if (node->arg_count == 0) {
                xemit_newarray(compiler->emitter, result_reg, 0, c_field);
            } else {
                int first_arg_reg = result_reg + 1;
                xreg_set_freereg(compiler->regalloc, first_arg_reg);
                compile_args_to_base(ctx, compiler, node->arguments, node->arg_count, first_arg_reg);
                xemit_newarray(compiler->emitter, result_reg, node->arg_count, c_field);
                xreg_set_freereg(compiler->regalloc, result_reg + 1);
            }
            return result_reg;
        }

        // new WeakSet() → OP_NEWSET with weak flag (B bit1 = weak)
        if (strcmp(class_name, TYPE_NAME_WEAKSET) == 0 && node->arg_count == 0) {
            int result_reg = reg_alloc(ctx, compiler);
            int b_field = ((int)ctx->current_elem_tid << 2) | 0x02;
            xemit_newset(compiler->emitter, result_reg, b_field);
            return result_reg;
        }

        // new Set() or new Set(a, b, c) -> OP_NEWSET
        if (strcmp(class_name, TYPE_NAME_SET) == 0) {
            int result_reg = reg_alloc(ctx, compiler);
            int b_field = ((int)ctx->current_elem_tid << 2) | ctx->current_storage_mode;
            if (node->arg_count == 0) {
                xemit_newset(compiler->emitter, result_reg, b_field);
            } else {
                int first_arg_reg = result_reg + 1;
                xreg_set_freereg(compiler->regalloc, first_arg_reg);
                compile_args_to_base(ctx, compiler, node->arguments, node->arg_count, first_arg_reg);
                xemit_newset(compiler->emitter, result_reg, b_field);
                xreg_set_freereg(compiler->regalloc, result_reg + 1);
            }
            return result_reg;
        }

        // new StringBuilder() → OP_NEWSTRINGBUILDER
        if (strcmp(class_name, TYPE_NAME_STRINGBUILDER) == 0 && node->arg_count == 0) {
            int result_reg = reg_alloc(ctx, compiler);
            xemit_newstringbuilder(compiler->emitter, result_reg, ctx->current_storage_mode);
            return result_reg;
        }

        // new Bytes(n) or new Bytes(n, value) -> OP_BYTES_NEW
        if (strcmp(class_name, TYPE_NAME_BYTES) == 0) {
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

        // new Channel(size) or new Channel(size, "name")
        if (strcmp(class_name, TYPE_NAME_CHANNEL) == 0) {
            // Compile size argument (supports runtime expressions)
            int result_reg = reg_alloc(ctx, compiler);

            if (node->arg_count == 0) {
                // new Channel() - unbuffered
                xemit_chan_new(compiler->emitter, result_reg, 0);
                return result_reg;
            }

            // Compile size expression to register
            XrExprDesc size_desc = xr_compile_expr(ctx, compiler, node->arguments[0]);
            int size_reg = xexpr_to_anyreg(ctx, compiler, &size_desc);
            if (size_reg < 0) return -1;

            // Named Channel: new Channel(size, "name") → OP_CHAN_NEW_NAMED
            if (node->arg_count >= 2) {
                XrExprDesc name_desc = xr_compile_expr(ctx, compiler, node->arguments[1]);
                int name_reg = xexpr_to_anyreg(ctx, compiler, &name_desc);
                if (name_reg < 0) return -1;
                xemit_chan_new_named(compiler->emitter, result_reg, size_reg, name_reg);
                reg_free(compiler, name_reg);
                reg_free(compiler, size_reg);
                return result_reg;
            }

            // Anonymous Channel with dynamic size → OP_CHAN_NEW_NAMED with null name
            int null_reg = reg_alloc(ctx, compiler);
            xemit_loadnull(compiler->emitter, null_reg);
            xemit_chan_new_named(compiler->emitter, result_reg, size_reg, null_reg);
            reg_free(compiler, null_reg);
            reg_free(compiler, size_reg);
            return result_reg;
        }

        // Value-type class: allocate struct in struct_area + call constructor with struct_ref
        // This avoids creating XrInstance for value types, which is incompatible with
        // OP_STRUCT_SET (expects struct_ref layout: [XrClass* 8B][fields...])
        if (ctx->class_registry) {
            ClassInfo *ci = xr_class_registry_lookup(ctx->class_registry, class_name);
            if (ci && ci->struct_layout) {
                XrStructLayout *layout = ci->struct_layout;

                // Allocate space in struct_area: 8B class ptr + field data, rounded to 16B
                int alloc_size = 8 + layout->total_size;
                int aligned_size = (alloc_size + 15) & ~15;
                int slot_offset = compiler->struct_area_offset / 16;
                compiler->struct_area_offset += aligned_size;

                // Track layout in proto for JIT
                XrProto *proto = compiler->emitter->proto;
                if (slot_offset >= proto->struct_layout_count) {
                    int new_count = slot_offset + 1;
                    proto->struct_layouts = (struct XrStructLayout**)xr_realloc(
                        proto->struct_layouts, new_count * sizeof(struct XrStructLayout*));
                    for (int si = proto->struct_layout_count; si < new_count; si++)
                        proto->struct_layouts[si] = NULL;
                    proto->struct_layout_count = new_count;
                }
                proto->struct_layouts[slot_offset] = layout;

                // R[base] = return value, R[base+1] = this (struct_ref), R[base+2..] = args
                int base = reg_alloc(ctx, compiler);

                // Load class to R[base+1], then OP_NEW_STRUCT overwrites with struct_ref
                // (VM reads class from B before writing struct_ref to A)
                load_class_to_reg(ctx, compiler, NULL, class_name, base + 1);
                xemit_new_struct(compiler->emitter, base + 1, base + 1, slot_offset);

                // Compile constructor arguments to R[base+2], R[base+3], ...
                int first_arg_reg = base + 2;
                xreg_set_freereg(compiler->regalloc, first_arg_reg);
                compile_args_to_base(ctx, compiler, node->arguments, node->arg_count, first_arg_reg);

                // Call constructor with struct_ref as receiver
                int global_sym = xr_symbol_register_in_table(
                    (XrSymbolTable*)xr_isolate_get_symbol_table(ctx->X), XR_KEYWORD_CONSTRUCTOR);
                int local_sym = emitter_add_symbol(compiler->emitter, global_sym);
                xemit_invoke(compiler->emitter, base, local_sym, node->arg_count);

                xreg_set_freereg(compiler->regalloc, base + 1);
                return base;
            }
        }
    }

    // ========== Generic path: constructor call (supports reflection, args, module classes) ==========

    // If storage mode context exists, generate OP_SET_STORAGE_CTX instruction first
    if (ctx->current_storage_mode != 0) {
        xemit_set_storage_ctx(compiler->emitter, ctx->current_storage_mode);
    }

    /*
     * Unified calling convention:
     *   R[base]   = return value position
     *   R[base+1] = this (Class object)
     *   R[base+2] = arg1
     *   R[base+3] = arg2
     *   ...
     */
    int base = reg_alloc(ctx, compiler);

    // Load class object to R[base+1]
    load_class_to_reg(ctx, compiler, node->module_name, node->class_name, base + 1);

    // Fix: set freereg to protect class register
    int first_arg_reg = base + 2;
    xreg_set_freereg(compiler->regalloc, first_arg_reg);

    // Compile constructor arguments to R[base+2], R[base+3], ...
    compile_args_to_base(ctx, compiler, node->arguments, node->arg_count, first_arg_reg);

    // Call constructor - register "constructor" in symbol system
    int global_sym = xr_symbol_register_in_table((XrSymbolTable*)xr_isolate_get_symbol_table(ctx->X), XR_KEYWORD_CONSTRUCTOR);
    int local_sym = emitter_add_symbol(compiler->emitter, global_sym);
    xemit_invoke(compiler->emitter, base, local_sym, node->arg_count);

    // Emit reified type args if present (e.g. new Box<int>(42))
    // Note: mono pass clears type_args for monomorphized classes, so this
    // is only reached for non-monomorphized (implicit inference) generics
    if (node->type_arg_count > 0) {
        int argc = node->type_arg_count > 3 ? 3 : node->type_arg_count;
        int tid0 = (argc >= 1) ? xr_type_to_tid(node->type_args[0]) : 0;
        int tid1 = (argc >= 2) ? xr_type_to_tid(node->type_args[1]) : 0;
        int packed = (argc & 0x03) | ((tid0 & 0x1F) << 2) | ((tid1 & 0x1F) << 7);
        xemit_inst_type_args(compiler->emitter, base, packed);
    }

    // After call, set freereg = base + 1 (keep return value in R[base])
    xreg_set_freereg(compiler->regalloc, base + 1);

    return base;
}

XrExprDesc compile_new_expr(XrCompilerContext *ctx, XrCompiler *compiler, NewExprNode *node) {
    XR_DCHECK(ctx != NULL, "compile_new_expr: NULL ctx");
    XR_DCHECK(compiler != NULL, "compile_new_expr: NULL compiler");
    XrExprDesc e = {0};
    int reg = compile_new_expr_internal(ctx, compiler, node);
    xexpr_init(&e, XEXPR_TEMP, reg);
    return e;
}

/*
 * Compile struct literal: Point{x: 1.0, y: 2.0}
 *
 * Fast path (OP_NEW_STRUCT + OP_STRUCT_SET):
 *   Allocate struct in per-frame struct_area (zero heap allocation).
 *   Native field access with known offsets from struct_layout.
 *
 * Fallback (OP_INVOKE + OP_SETPROP):
 *   When ClassRegistry info is unavailable (e.g. imported classes).
 */
XrExprDesc compile_struct_literal(XrCompilerContext *ctx, XrCompiler *compiler, StructLiteralNode *node) {
    XR_DCHECK(ctx != NULL, "compile_struct_literal: NULL ctx");
    XR_DCHECK(compiler != NULL, "compile_struct_literal: NULL compiler");
    XrExprDesc e = {0};

    // Try fast path: check if all field indices are known at compile time
    ClassInfo *class_info = NULL;
    if (ctx->class_registry) {
        class_info = xr_class_registry_lookup(ctx->class_registry, node->struct_name);
    }

    bool use_fast_path = false;
    if (class_info) {
        use_fast_path = true;
        for (int i = 0; i < node->field_count; i++) {
            if (xr_class_find_instance_field_index(class_info, node->field_names[i]) < 0) {
                use_fast_path = false;
                break;
            }
        }
    }

    // Fast path: stack-allocated struct (native storage)
    if (use_fast_path && class_info->struct_layout) {
        XrStructLayout *layout = class_info->struct_layout;

        // Allocate space in struct_area: 8B class ptr + field data, rounded to 16B
        int alloc_size = 8 + layout->total_size;
        int aligned_size = (alloc_size + 15) & ~15;
        int slot_offset = compiler->struct_area_offset / 16;
        compiler->struct_area_offset += aligned_size;

        int class_reg = reg_alloc(ctx, compiler);
        load_class_to_reg(ctx, compiler, NULL, node->struct_name, class_reg);

        // Store struct_layout in proto for JIT: indexed by slot_offset
        XrProto *proto = compiler->emitter->proto;
        if (slot_offset >= proto->struct_layout_count) {
            int new_count = slot_offset + 1;
            proto->struct_layouts = (struct XrStructLayout**)xr_realloc(
                proto->struct_layouts, new_count * sizeof(struct XrStructLayout*));
            for (int si = proto->struct_layout_count; si < new_count; si++)
                proto->struct_layouts[si] = NULL;
            proto->struct_layout_count = new_count;
        }
        proto->struct_layouts[slot_offset] = layout;

        int obj_reg = reg_alloc(ctx, compiler);
        xemit_new_struct(compiler->emitter, obj_reg, class_reg, slot_offset);
        reg_free(compiler, class_reg);

        for (int i = 0; i < node->field_count; i++) {
            XrExprDesc val_expr = xr_compile_expr(ctx, compiler, node->field_values[i]);
            int value_reg = xexpr_to_anyreg(ctx, compiler, &val_expr);

            int field_idx = xr_class_find_instance_field_index(class_info, node->field_names[i]);
            xemit_struct_set(compiler->emitter, obj_reg, field_idx, value_reg);
            reg_free(compiler, value_reg);
        }

        xexpr_init(&e, XEXPR_TEMP, obj_reg);
        return e;
    }

    // Fallback: OP_INVOKE("constructor") + OP_SETPROP
    NewExprNode new_node = {0};
    new_node.class_name = node->struct_name;
    new_node.module_name = NULL;
    new_node.arguments = NULL;
    new_node.arg_count = 0;
    new_node.type_args = NULL;
    new_node.type_arg_count = 0;

    int obj_reg = compile_new_expr_internal(ctx, compiler, &new_node);

    for (int i = 0; i < node->field_count; i++) {
        XrExprDesc val_expr = xr_compile_expr(ctx, compiler, node->field_values[i]);
        int value_reg = xexpr_to_anyreg(ctx, compiler, &val_expr);

        int global_sym = xr_symbol_register_in_table(
            (XrSymbolTable*)xr_isolate_get_symbol_table(ctx->X), node->field_names[i]);
        int local_sym = emitter_add_symbol(compiler->emitter, global_sym);
        xemit_setprop(compiler->emitter, obj_reg, local_sym, value_reg);
        reg_free(compiler, value_reg);
    }

    xexpr_init(&e, XEXPR_TEMP, obj_reg);
    return e;
}


// ========== Member Access ==========

/*
 * Internal implementation: compile member access (returns register)
 */
static int compile_member_access_internal(XrCompilerContext *ctx, XrCompiler *compiler, MemberAccessNode *node, XrType **out_compile_type, bool *out_is_raw) {
    // Compile-time constants: Coro.LOW/NORMAL/HIGH, Type.xxx
    if (node->object->type == AST_VARIABLE) {
        const char *obj_name = node->object->as.variable.name;
        if (strcmp(obj_name, GLOBAL_NAME_CORO) == 0) {
            int prio_val = -1;
            if (strcmp(node->name, "LOW") == 0) {
                prio_val = 0;  // CORO_PRIORITY_LOW
            } else if (strcmp(node->name, "NORMAL") == 0) {
                prio_val = 1;  // CORO_PRIORITY_NORMAL
            } else if (strcmp(node->name, "HIGH") == 0) {
                prio_val = 2;  // CORO_PRIORITY_HIGH
            }
            if (prio_val >= 0) {
                int pc = xemit_loadi(compiler->emitter, 0, prio_val);
                return pc;
            }
        }
        // Type.xxx constants → compile to integer XrTypeId
        if (strcmp(obj_name, CLASS_NAME_TYPE) == 0) {
            static const struct { const char *name; int tid; } type_consts[] = {
                {"null",          XR_TID_NULL},
                {"bool",          XR_TID_BOOL},
                {"int8",          XR_TID_INT8},
                {"uint8",         XR_TID_UINT8},
                {"int16",         XR_TID_INT16},
                {"uint16",        XR_TID_UINT16},
                {"int32",         XR_TID_INT32},
                {"uint32",        XR_TID_UINT32},
                {"int",           XR_TID_INT},
                {"int64",         XR_TID_INT},
                {"uint64",        XR_TID_UINT64},
                {"float32",       XR_TID_FLOAT32},
                {"float",         XR_TID_FLOAT},
                {"float64",       XR_TID_FLOAT},
                {"string",        XR_TID_STRING},
                {"function",      XR_TID_FUNCTION},
                {"Array",         XR_TID_ARRAY},
                {"Set",           XR_TID_SET},
                {"Map",           XR_TID_MAP},
                {"instance",      XR_TID_INSTANCE},
                {"Json",          XR_TID_JSON},
                {"BigInt",        XR_TID_BIGINT},
                {"StringBuilder", XR_TID_STRINGBUILDER},
                {"Channel",       XR_TID_CHANNEL},
                {"Regex",         XR_TID_REGEX},
                {"DateTime",      XR_TID_DATETIME},
                {"Exception",     XR_TID_EXCEPTION},
                {"enum_value",    XR_TID_ENUM_VALUE},
                {"enum_type",     XR_TID_ENUM_TYPE},
            };
            for (int ti = 0; ti < (int)(sizeof(type_consts)/sizeof(type_consts[0])); ti++) {
                if (strcmp(node->name, type_consts[ti].name) == 0) {
                    int pc = xemit_loadi(compiler->emitter, 0, type_consts[ti].tid);
                    // OP_LOADI for Type constants produces raw I64
                    if (out_compile_type) *out_compile_type = xr_type_new_int(NULL);
                    if (out_is_raw) *out_is_raw = true;
                    return pc;
                }
            }
            xr_compiler_error(ctx, compiler, "Type has no member '%s'", node->name);
        }
    }

    // Instance field access optimization (using frontend type inference)
    // Priority: local->compile_type (set by Analyzer-inferred param types or annotations)
    //         > xa_analyzer_get_node_type(ctx->analyzer, node->object)
    //           (set by Analyzer expression inference, side table)
    XrType *obj_type = NULL;

    if (node->object->type == AST_VARIABLE) {
        const char *var_name = node->object->as.variable.name;

        // Check local variable's compile_type first
        for (int i = compiler->local_list.count - 1; i >= 0; i--) {
            XrLocalInfo *local = compiler->local_list.items[i];
            if (local->name && strcmp(local->name->data, var_name) == 0) {
                if (local->compile_type)
                    obj_type = local->compile_type;
                break;
            }
        }

        // Fallback to analyzer-inferred type if local type is absent or
        // generic. The inferred type is read via the analyzer side table.
        if (!obj_type || XR_TYPE_IS_UNKNOWN(obj_type)) {
            XrType *ast_type = xa_analyzer_get_node_type(ctx->analyzer, node->object);
            if (ast_type && !XR_TYPE_IS_UNKNOWN(ast_type))
                obj_type = ast_type;
        }
    } else {
        // Non-variable: use analyzer-inferred type from the side table.
        XrType *ast_type = xa_analyzer_get_node_type(ctx->analyzer, node->object);
        if (ast_type)
            obj_type = ast_type;
    }


    // CT_JSON (unified object type): behavior depends on allow_extension
    if (obj_type && obj_type->kind == XR_KIND_JSON) {
        // Search in known fields (skip NULL names from computed properties)
        int field_idx = -1;
        int field_orig_idx = -1;
        int static_idx = 0;
        for (int i = 0; i < obj_type->object.field_count; i++) {
            if (obj_type->object.field_names[i]) {
                if (strcmp(obj_type->object.field_names[i], node->name) == 0) {
                    field_idx = static_idx;
                    field_orig_idx = i;
                    break;
                }
                static_idx++;
            }
        }

        XrExprDesc obj_expr = xr_compile_expr(ctx, compiler, node->object);
        int obj_reg = xexpr_to_anyreg_readonly(ctx, compiler, &obj_expr);

        if (field_idx >= 0) {
            // Strict type alias (allow_extension=false): shape is stable,
            // safe to use OP_TFIELD_GET for typed primitive fields
            if (!obj_type->object.allow_extension && obj_type->object.field_types) {
                XrType *ft = (field_orig_idx >= 0)
                           ? obj_type->object.field_types[field_orig_idx] : NULL;
                if (ft && (ft->kind == XR_KIND_INT || ft->kind == XR_KIND_FLOAT)) {
                    // Record float field in bitmap so JIT uses FP register (XR_REP_F64)
                    if (ft && (ft->kind == XR_KIND_FLOAT) && field_idx < 64) {
                        compiler->emitter->proto->tfield_float_bitmap |= (uint64_t)1 << field_idx;
                    }
                    int pc = xemit_tfield_get(compiler->emitter, 0, obj_reg, field_idx);
                    reg_free(compiler, obj_reg);
                    // TFIELD_GET now outputs tagged values — no slot_type override
                    return pc;
                }
            }
            // Known field: use OP_JSON_GET (O(1) direct index)
            int pc = xemit_json_get(compiler->emitter, 0, obj_reg, field_idx);
            reg_free(compiler, obj_reg);
            return pc;
        } else if (obj_type->object.allow_extension) {
            // Allow extension: use OP_JSON_GETK (dynamic Symbol lookup)
            XrSymbolTable *sym_table = (XrSymbolTable*)xr_isolate_get_symbol_table(ctx->X);
            int global_sym = xr_symbol_register_in_table(sym_table, node->name);
            int local_sym = emitter_add_symbol(compiler->emitter, global_sym);
            int pc = xemit_json_getk(compiler->emitter, 0, obj_reg, local_sym);
            reg_free(compiler, obj_reg);
            return pc;
        } else {
            // No extension allowed (strict type): field doesn't exist, report compile error
            reg_free(compiler, obj_reg);
            const char *type_name = obj_type->object.type_name;
            if (!type_name) type_name = "type";
            xr_compiler_error(ctx, compiler,
                "Type '%s' has no field '%s'",
                type_name, node->name);
            return reg_alloc(ctx, compiler);
        }
    }

    if (obj_type && (obj_type->kind == XR_KIND_CLASS || obj_type->kind == XR_KIND_INSTANCE) && ctx->class_registry) {
        // Type is known! Get class name from compile-time type
        const char *class_name = obj_type->instance.class_name;

        if (class_name) {
            ClassInfo *class_info = xr_class_registry_lookup(
                ctx->class_registry, class_name);

            if (class_info) {
                // Find instance field index
                int field_idx = xr_class_find_instance_field_index(
                    class_info, node->name);

                if (field_idx >= 0) {
                    XrExprDesc obj_expr = xr_compile_expr(ctx, compiler,
                                                          node->object);
                    int obj_reg = xexpr_to_anyreg(ctx, compiler, &obj_expr);
                    int pc;

                    // Struct: native field read
                    if (class_info->struct_layout) {
                        pc = xemit_struct_get(compiler->emitter, 0, obj_reg, field_idx);
                        reg_free(compiler, obj_reg);
                        return pc;
                    }

                    // Class instance: heap field access
                    uint8_t fst = xr_class_get_field_slot_type(
                        class_info, node->name);
                    if (fst == 7 || fst == 10) {
                        // XR_SLOT_I64(7) or XR_SLOT_F64(10): use OP_TFIELD_GET
                        if (fst == 10 && field_idx < 64)
                            compiler->emitter->proto->tfield_float_bitmap |=
                                (uint64_t)1 << field_idx;
                        pc = xemit_tfield_get(compiler->emitter, 0, obj_reg, field_idx);
                    } else {
                        pc = xemit_getfield(compiler->emitter, 0, obj_reg, field_idx);
                    }
                    reg_free(compiler, obj_reg);
                    return pc;
                }
            }
        }
    }

    // Optimization: this.field access (compile-time type known)
    if (ctx->current_class_desc != NULL &&
        node->object->type == AST_THIS_EXPR) {

        // Find field index in current class
        XrClassDescriptor *desc = ctx->current_class_desc;
        int field_idx = -1;

        // Use ClassInfo which has correct field indices including parent offset
        if (ctx->class_registry) {
            ClassInfo *class_info = xr_class_registry_lookup(ctx->class_registry, desc->class_name);
            if (class_info) {
                field_idx = xr_class_find_instance_field_index(class_info, node->name);
            }
        }

        if (field_idx >= 0) {
            ClassInfo *ci = xr_class_registry_lookup(ctx->class_registry, desc->class_name);
            XrExprDesc obj_expr = xr_compile_expr(ctx, compiler, node->object);
            int obj_reg = xexpr_to_anyreg_readonly(ctx, compiler, &obj_expr);
            int pc;

            // Struct: native field read
            if (ci && ci->struct_layout) {
                pc = xemit_struct_get(compiler->emitter, 0, obj_reg, field_idx);
                return pc;
            }

            // Class instance: heap field access
            uint8_t fst = ci ? xr_class_get_field_slot_type(ci, node->name) : 0;
            if (fst == 7 || fst == 10) {
                // XR_SLOT_I64(7) or XR_SLOT_F64(10): use OP_TFIELD_GET
                if (fst == 10 && field_idx < 64)
                    compiler->emitter->proto->tfield_float_bitmap |=
                        (uint64_t)1 << field_idx;
                pc = xemit_tfield_get(compiler->emitter, 0, obj_reg, field_idx);
            } else {
                pc = xemit_getfield(compiler->emitter, 0, obj_reg, field_idx);
            }
            return pc;
        }
    }


    // Compile object expression (fallback path) - use readonly to avoid redundant MOVE
    XrExprDesc obj_expr = xr_compile_expr(ctx, compiler, node->object);
    int obj_reg = xexpr_to_anyreg_readonly(ctx, compiler, &obj_expr);

    // Fallback: runtime handling
    // Convert property name to Symbol
    int global_sym = xr_symbol_register_in_table((XrSymbolTable*)xr_isolate_get_symbol_table(ctx->X), node->name);
    int local_sym = emitter_add_symbol(compiler->emitter, global_sym);

    // RELOC optimization: A=0 pending relocation
    int pc = xemit_getprop(compiler->emitter, 0, obj_reg, local_sym);

    // readonly version reuses original register, no need to free

    // Return pc for subsequent writeback
    return pc;
}

XrExprDesc compile_member_access(XrCompilerContext *ctx, XrCompiler *compiler, MemberAccessNode *node) {
    XR_DCHECK(ctx != NULL, "compile_member_access: NULL ctx");
    XR_DCHECK(compiler != NULL, "compile_member_access: NULL compiler");
    XrExprDesc e = {0};
    XrType *ct = NULL;
    bool is_raw = false;
    int pc = compile_member_access_internal(ctx, compiler, node, &ct, &is_raw);
    xexpr_init(&e, XEXPR_RELOC, -1);
    e.u.pc = pc;
    e.compile_type = ct;
    e.is_raw = is_raw;
    return e;
}


