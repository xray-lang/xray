/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xstmt_assignment.c - Xray assignment statement compilation
 *
 * KEY CONCEPT:
 *   Handle complex assignment statements:
 *   - Index assignment (a[b] = c)
 *   - Member assignment (obj.member = value)
 */

#include "xstmt.h"
#include "../../base/xchecks.h"
#include "../../runtime/xisolate_api.h"  // XrayIsolate full definition
#include "xcompiler.h"
#include "xcompiler_context.h"
#include "xemit.h"
#include "xexpr.h"                                  // xr_compile_expr
#include "xexpr_desc.h"                             // XrExprDesc
#include "../../runtime/symbol/xsymbol_table.h"     // unified symbol table system
#include "../../runtime/class/xclass_descriptor.h"  // XrClassDescriptor
#include "xcompiler_class_registry.h"               // ClassInfo, xr_class_registry_lookup
#include "../../runtime/value/xtype.h"
#include "../analyzer/xanalyzer.h"
#include "../../runtime/object/xstring.h"  // xr_string_intern, xr_string_value
#include "../../runtime/value/xstruct_layout.h"
#include <stdio.h>
#include <string.h>

// Detect typed array element slot type from compile-time type info.
static uint8_t get_typed_array_elem_slot_for_set(XrCompilerContext *ctx, AstNode *array_node) {
    if (!ctx->analyzer || !array_node)
        return XR_SLOT_ANY;
    XrType *arr_type = xa_analyzer_infer_expr_type(ctx->analyzer, array_node);
    if (!arr_type || !(arr_type->kind == XR_KIND_ARRAY))
        return XR_SLOT_ANY;
    XrType *elem = arr_type->container.element_type;
    if (!elem)
        return XR_SLOT_ANY;
    if (elem->kind == XR_KIND_INT)
        return XR_SLOT_I64;
    if (elem->kind == XR_KIND_FLOAT)
        return XR_SLOT_F64;
    return XR_SLOT_ANY;
}

// New symbol system doesn't need global variables, xr_symbol_register/get_name are global functions

// ========== Index Assignment ==========

/*
 * Compile index assignment statement
 *
 * a[b] = c -> INDEX_SET a, b, c
 */
void compile_index_set(XrCompilerContext *ctx, XrCompiler *compiler, IndexSetNode *node) {
    XR_DCHECK(ctx != NULL, "compile_index_set: NULL ctx");
    XR_DCHECK(compiler != NULL, "compile_index_set: NULL compiler");
    // const check: cannot modify elements of const container
    if (ctx->analyzer && node->array) {
        XrType *arr_type = xa_analyzer_infer_expr_type(ctx->analyzer, node->array);
        if (arr_type && xr_type_is_const(arr_type) && !xr_type_is_inherently_immutable(arr_type)) {
            int saved_line = ctx->current_line;
            ctx->current_line = node->array->line;
            xr_compiler_error(ctx, compiler, "cannot modify elements of const container");
            ctx->current_line = saved_line;
            return;
        }
    }

    // Compile array expression - use readonly version to avoid redundant MOVE
    XrExprDesc arr_expr = xr_compile_expr(ctx, compiler, node->array);
    int array_reg = xexpr_to_anyreg_readonly(ctx, compiler, &arr_expr);

    // Optimization: use OP_ARRAY_SETC for integer literal index (e.g. arr[0] = v)
    if (node->index->type == AST_LITERAL_INT) {
        LiteralNode *lit = (LiteralNode *) &node->index->as;
        int64_t idx = lit->raw_value.int_val;

        // B parameter only has 8 bits (0-255), check range
        if (idx >= 0 && idx <= 255) {
            uint8_t elem_slot = get_typed_array_elem_slot_for_set(ctx, node->array);
            if (elem_slot != XR_SLOT_ANY) {
                // Typed array: OP_TARRAY_SET R[A]:arr[R[B]] = R[C].i
                int idx_reg = reg_alloc(ctx, compiler);
                xemit_loadi(compiler->emitter, idx_reg, (int) idx);
                XrExprDesc val_expr = xr_compile_expr(ctx, compiler, node->value);
                int value_reg = xexpr_to_anyreg_readonly(ctx, compiler, &val_expr);
                xemit_tarray_set(compiler->emitter, array_reg, idx_reg, value_reg);
                reg_free(compiler, value_reg);
                reg_free(compiler, idx_reg);
                reg_free(compiler, array_reg);
                return;
            }
            // Compile value expression
            XrExprDesc val_expr = xr_compile_expr(ctx, compiler, node->value);
            int value_reg = xexpr_to_anyreg(ctx, compiler, &val_expr);

            // OP_ARRAY_SETC A B C: R[A][B] = R[C]
            xemit_array_setc(compiler->emitter, array_reg, (int) idx, value_reg);

            reg_free(compiler, value_reg);
            reg_free(compiler, array_reg);
            return;
        }
    }

    // Optimization: use OP_MAP_SETK for string literal key (e.g. map["key"] = v)
    if (node->index->type == AST_LITERAL_STRING) {
        LiteralNode *lit = (LiteralNode *) &node->index->as;
        const char *str_val = lit->raw_value.string_val;
        // Use interned string, share same object with Map key
        XrString *key_str = xr_compile_time_intern(ctx->X, str_val, strlen(str_val));
        int key_const = xr_vm_proto_add_constant(compiler->proto, xr_string_value(key_str));

        // Compile value expression
        XrExprDesc val_expr = xr_compile_expr(ctx, compiler, node->value);
        int value_reg = xexpr_to_anyreg(ctx, compiler, &val_expr);

        // OP_MAP_SETK A B C: R[A][K[B]] = R[C]
        xemit_map_setk(compiler->emitter, array_reg, key_const, value_reg);

        reg_free(compiler, value_reg);
        reg_free(compiler, array_reg);
        return;
    }

    // General path: compile index expression
    XrExprDesc idx_expr = xr_compile_expr(ctx, compiler, node->index);
    int index_reg = xexpr_to_anyreg_readonly(ctx, compiler, &idx_expr);

    // Typed array fast path: OP_TARRAY_SET (raw in, no BOX)
    {
        uint8_t elem_slot = get_typed_array_elem_slot_for_set(ctx, node->array);
        if (elem_slot != XR_SLOT_ANY) {
            XrExprDesc val_expr = xr_compile_expr(ctx, compiler, node->value);
            int value_reg = xexpr_to_anyreg_readonly(ctx, compiler, &val_expr);
            xemit_tarray_set(compiler->emitter, array_reg, index_reg, value_reg);
            reg_free(compiler, value_reg);
            reg_free(compiler, index_reg);
            reg_free(compiler, array_reg);
            return;
        }
    }

    // Compile value expression
    XrExprDesc val_expr = xr_compile_expr(ctx, compiler, node->value);
    int value_reg = xexpr_to_anyreg(ctx, compiler, &val_expr);

    // Use generic INDEX_SET instruction
    xemit_index_set(compiler->emitter, array_reg, index_reg, value_reg);

    reg_free(compiler, index_reg);
    reg_free(compiler, value_reg);
    reg_free(compiler, array_reg);
}

// ========== Member Assignment ==========

/*
 * Check if there are readonly fields in member access chain
 * e.g. user.profile.avatar: if profile is const, return true
 *
 * @param ctx       Compiler context
 * @param compiler  Compiler
 * @param expr      Access expression
 * @param out_field Output: readonly field name (for error message)
 * @param out_type  Output: type name containing the field
 * @return          true if there are readonly fields in access chain
 */
static bool check_readonly_chain(XrCompilerContext *ctx, XrCompiler *compiler, AstNode *expr,
                                 const char **out_field, const char **out_type) {
    if (expr->type != AST_MEMBER_ACCESS) {
        return false;
    }

    MemberAccessNode *member = &expr->as.member_access;

    // Get object type
    XrType *obj_type = NULL;

    // Read the inferred type via the analyzer side table.
    XrType *member_obj_t = xa_analyzer_get_node_type(ctx->analyzer, member->object);
    if (member_obj_t) {
        obj_type = member_obj_t;
    } else if (ctx->analyzer && member->object->type == AST_VARIABLE) {
        const char *var_name = member->object->as.variable.name;
        obj_type = xa_analyzer_lookup_var(ctx->analyzer, var_name);
    }

    // Check if current field is readonly (const modified)
    if (obj_type && obj_type->kind == XR_KIND_JSON) {
        for (int i = 0; i < obj_type->object.field_count; i++) {
            if (obj_type->object.field_names[i] &&
                strcmp(obj_type->object.field_names[i], member->name) == 0) {
                if (obj_type->object.field_readonly && obj_type->object.field_readonly[i]) {
                    *out_field = member->name;
                    *out_type = obj_type->object.type_name ? obj_type->object.type_name : "object";
                    return true;
                }
                break;
            }
        }
    }

    // Recursively check parent access
    return check_readonly_chain(ctx, compiler, member->object, out_field, out_type);
}

/*
 * Compile member assignment statement
 *
 * obj.member = value -> SETPROP obj, symbol, value
 *
 * Optimization paths (by priority):
 * 1. this.field -> SETFIELD_FAST (in class method)
 * 2. struct.field -> STRUCT_SET (Struct type)
 * 3. Fallback -> SETPROP (runtime lookup)
 */
void compile_member_set(XrCompilerContext *ctx, XrCompiler *compiler, MemberSetNode *node) {
    XR_DCHECK(ctx != NULL, "compile_member_set: NULL ctx");
    XR_DCHECK(compiler != NULL, "compile_member_set: NULL compiler");
    // Check if root object is const (const objects are fully immutable)
    AstNode *root = node->object;
    while (root->type == AST_MEMBER_ACCESS) {
        root = root->as.member_access.object;
    }
    if (root->type == AST_VARIABLE) {
        XrLocalInfo *local = compiler_get_local_by_name(compiler, root->as.variable.name);
        if (local && local->is_const) {
            xr_compiler_error(ctx, compiler, "cannot modify field '%s' of const object '%s'",
                              node->member, root->as.variable.name);
            return;
        }
    }

    // Nested readonly check: if profile is const in user.profile.avatar
    const char *readonly_field = NULL;
    const char *readonly_type = NULL;
    if (check_readonly_chain(ctx, compiler, node->object, &readonly_field, &readonly_type)) {
        xr_compiler_error(ctx, compiler,
                          "cannot modify subfield of '%s.%s': '%s' is readonly (const modified)",
                          readonly_type, readonly_field, readonly_field);
        return;
    }

    // Optimization 1: check if this.field access (in class method)
    if (ctx->current_class_desc != NULL && node->object->type == AST_THIS_EXPR &&
        ctx->class_registry) {
        // Use ClassInfo which already has correct field indices (including parent offset)
        XrClassDescriptor *desc = ctx->current_class_desc;
        ClassInfo *class_info = xr_class_registry_lookup(ctx->class_registry, desc->class_name);
        int field_idx =
            class_info ? xr_class_find_instance_field_index(class_info, node->member) : -1;

        if (field_idx >= 0) {
            XrExprDesc obj_expr = xr_compile_expr(ctx, compiler, node->object);
            int obj_reg = xexpr_to_anyreg_readonly(ctx, compiler, &obj_expr);
            XrExprDesc val_expr = xr_compile_expr(ctx, compiler, node->value);
            int value_reg = xexpr_to_anyreg(ctx, compiler, &val_expr);

            // Struct: native field write
            if (class_info && class_info->struct_layout) {
                xemit_struct_set(compiler->emitter, obj_reg, field_idx, value_reg);
                reg_free(compiler, value_reg);
                return;
            }

            // Class instance: heap field write
            uint8_t fst = class_info ? xr_class_get_field_slot_type(class_info, node->member) : 0;
            if (fst == 7 || fst == 10) {
                // XR_SLOT_I64(7) or XR_SLOT_F64(10): use OP_TFIELD_SET
                if (fst == 10 && field_idx < 64)
                    compiler->emitter->proto->tfield_float_bitmap |= (uint64_t) 1 << field_idx;
                xemit_tfield_set(compiler->emitter, obj_reg, field_idx, value_reg);
            } else {
                xemit_setfield(compiler->emitter, obj_reg, field_idx, value_reg);
            }
            reg_free(compiler, value_reg);
            return;
        }
    }

    // Optimization 2: Struct field access (compile-time type inference)
    XrType *obj_type = NULL;

    // Prefer expression node's inferred type (convert XrType* to XrType*).
    // Read it via the analyzer side table.
    XrType *node_obj_t = xa_analyzer_get_node_type(ctx->analyzer, node->object);
    if (node_obj_t) {
        obj_type = node_obj_t;
    }
    // Search local_list for compile_type (matches read path in xexpr_object.c)
    else if (node->object->type == AST_VARIABLE) {
        const char *var_name = node->object->as.variable.name;
        for (int i = compiler->local_list.count - 1; i >= 0; i--) {
            XrLocalInfo *local = compiler->local_list.items[i];
            if (local->name && strcmp(local->name->data, var_name) == 0) {
                if (local->compile_type) {
                    obj_type = (XrType *) (local->compile_type);
                }
                break;
            }
        }
        // Fallback: lookup from analyzer
        if (!obj_type && ctx->analyzer) {
            obj_type = xa_analyzer_lookup_var(ctx->analyzer, var_name);
        }
    }

    // CT_JSON / CT_OBJECT: decide behavior based on type kind
    if (obj_type && (obj_type->kind == XR_KIND_JSON || obj_type->kind == XR_KIND_OBJECT)) {
        int field_idx = -1;
        int field_orig_idx = -1;
        int static_idx = 0;
        for (int i = 0; i < obj_type->object.field_count; i++) {
            if (obj_type->object.field_names[i]) {
                if (strcmp(obj_type->object.field_names[i], node->member) == 0) {
                    field_idx = static_idx;
                    field_orig_idx = i;
                    break;
                }
                static_idx++;
            }
        }

        if (field_idx >= 0) {
            // Check if field is readonly (const modified)
            if (obj_type->object.field_readonly && obj_type->object.field_readonly[field_idx]) {
                const char *type_name = obj_type->object.type_name;
                if (!type_name)
                    type_name = "object";
                xr_compiler_error(ctx, compiler,
                                  "cannot modify readonly field '%s.%s' (const modified)",
                                  type_name, node->member);
                return;
            }

            // OBJECT kind: shape is stable, safe to use OP_TFIELD_SET for typed fields
            if (obj_type->kind == XR_KIND_OBJECT && obj_type->object.field_types) {
                XrType *ft =
                    (field_orig_idx >= 0) ? obj_type->object.field_types[field_orig_idx] : NULL;
                bool use_tfield = false;
                if (ft && (ft->kind == XR_KIND_INT || ft->kind == XR_KIND_FLOAT))
                    use_tfield = true;
                if (use_tfield) {
                    // Record float field in bitmap so JIT uses FP register (XR_REP_F64)
                    if (ft && (ft->kind == XR_KIND_FLOAT) && field_idx < 64) {
                        compiler->emitter->proto->tfield_float_bitmap |= (uint64_t) 1 << field_idx;
                    }
                    XrExprDesc obj_expr = xr_compile_expr(ctx, compiler, node->object);
                    int obj_reg = xexpr_to_anyreg_readonly(ctx, compiler, &obj_expr);
                    XrExprDesc val_expr = xr_compile_expr(ctx, compiler, node->value);
                    int value_reg = xexpr_to_anyreg_readonly(ctx, compiler, &val_expr);
                    xemit_tfield_set(compiler->emitter, obj_reg, field_idx, value_reg);
                    reg_free(compiler, value_reg);
                    reg_free(compiler, obj_reg);
                    return;
                }
            }

            // Known field: use OP_JSON_SET (O(1) direct index)
            XrExprDesc obj_expr = xr_compile_expr(ctx, compiler, node->object);
            int obj_reg = xexpr_to_anyreg_readonly(ctx, compiler, &obj_expr);
            XrExprDesc val_expr = xr_compile_expr(ctx, compiler, node->value);
            int value_reg = xexpr_to_anyreg(ctx, compiler, &val_expr);

            xemit_json_set(compiler->emitter, obj_reg, field_idx, value_reg);
            reg_free(compiler, value_reg);
            reg_free(compiler, obj_reg);
            return;
        } else if (obj_type->kind == XR_KIND_JSON) {
            // Json is extensible: use OP_JSON_SETK (dynamic add, Shape transition)
            XrExprDesc obj_expr = xr_compile_expr(ctx, compiler, node->object);
            int obj_reg = xexpr_to_anyreg_readonly(ctx, compiler, &obj_expr);
            XrExprDesc val_expr = xr_compile_expr(ctx, compiler, node->value);
            int value_reg = xexpr_to_anyreg(ctx, compiler, &val_expr);

            int global_sym = xr_symbol_register_in_table(
                (XrSymbolTable *) xr_isolate_get_symbol_table(ctx->X), node->member);
            int local_sym = emitter_add_symbol(compiler->emitter, global_sym);
            xemit_json_setk(compiler->emitter, obj_reg, local_sym, value_reg);
            reg_free(compiler, value_reg);
            reg_free(compiler, obj_reg);
            return;
        } else {
            // Disallow extension (strict type): not allowed to add new field
            const char *type_name = obj_type->object.type_name;
            if (!type_name)
                type_name = "type";
            xr_compiler_error(ctx, compiler, "type '%s' does not allow adding field '%s'",
                              type_name, node->member);
            return;
        }
    }

    /*
     * Optimization 3: user-defined class instance field access (compile-time type inference)
     * Design: type is inferred via get_expr_type() at let declaration and saved to
     * local->compile_type Here directly use saved type info to generate SETFIELD_FAST, no runtime
     * lookup needed
     */
    if (ctx->class_registry && node->object->type == AST_VARIABLE) {
        const char *var_name = node->object->as.variable.name;

        // Get compile-time type info from local variable
        XrLocalInfo *local = compiler_get_local_by_name(compiler, var_name);
        if (local && local->compile_type) {
            XrType *ct = (XrType *) (local->compile_type);
            if ((ct->kind == XR_KIND_CLASS || ct->kind == XR_KIND_INSTANCE) &&
                ct->instance.class_name) {
                const char *class_name = ct->instance.class_name;
                // Lookup class field layout from ClassRegistry
                ClassInfo *class_info = xr_class_registry_lookup(ctx->class_registry, class_name);
                if (class_info && class_info->instance_field_count > 0) {
                    // Find target field index in instance fields
                    int field_idx = -1;
                    for (int i = 0; i < class_info->instance_field_count; i++) {
                        if (class_info->instance_fields[i].name &&
                            strcmp(class_info->instance_fields[i].name, node->member) == 0) {
                            field_idx = class_info->instance_fields[i].index;
                            break;
                        }
                    }

                    if (field_idx >= 0) {
                        XrExprDesc obj_expr = xr_compile_expr(ctx, compiler, node->object);
                        int obj_reg = xexpr_to_anyreg_readonly(ctx, compiler, &obj_expr);
                        XrExprDesc val_expr = xr_compile_expr(ctx, compiler, node->value);
                        int value_reg = xexpr_to_anyreg_readonly(ctx, compiler, &val_expr);

                        // Struct: native field write
                        if (class_info->struct_layout) {
                            xemit_struct_set(compiler->emitter, obj_reg, field_idx, value_reg);
                            reg_free(compiler, value_reg);
                            reg_free(compiler, obj_reg);
                            return;
                        }

                        // Class instance: heap field write
                        uint8_t fst = xr_class_get_field_slot_type(class_info, node->member);
                        if (fst == 7 || fst == 10) {  // XR_SLOT_I64 or XR_SLOT_F64
                            if (fst == 10 && field_idx < 64)
                                compiler->emitter->proto->tfield_float_bitmap |= (uint64_t) 1
                                                                                 << field_idx;
                            xemit_tfield_set(compiler->emitter, obj_reg, field_idx, value_reg);
                        } else {
                            xemit_setfield(compiler->emitter, obj_reg, field_idx, value_reg);
                        }
                        reg_free(compiler, value_reg);
                        reg_free(compiler, obj_reg);
                        return;
                    }
                }
            }
        }
    }

    /*
     * Optimization 4: Nested struct field write via member access chain
     * Handles: r.origin.x = 99.0 where r.origin returns a nested struct_ref
     * Uses compile_type set by analyzer on intermediate MemberAccess nodes
     */
    // Read the inferred type via the analyzer side table.
    XrType *node_obj_ct = xa_analyzer_get_node_type(ctx->analyzer, node->object);
    if (ctx->class_registry && node_obj_ct) {
        XrType *obj_ct = node_obj_ct;
        ClassInfo *ci = NULL;
        if ((obj_ct->kind == XR_KIND_CLASS || obj_ct->kind == XR_KIND_INSTANCE) &&
            obj_ct->instance.class_name)
            ci = xr_class_registry_lookup(ctx->class_registry, obj_ct->instance.class_name);
        if (ci && ci->struct_layout) {
            int field_idx = xr_class_find_instance_field_index(ci, node->member);
            if (field_idx >= 0) {
                XrExprDesc oe = xr_compile_expr(ctx, compiler, node->object);
                int obj_reg = xexpr_to_anyreg(ctx, compiler, &oe);
                XrExprDesc ve = xr_compile_expr(ctx, compiler, node->value);
                int val_reg = xexpr_to_anyreg(ctx, compiler, &ve);
                xemit_struct_set(compiler->emitter, obj_reg, field_idx, val_reg);
                reg_free(compiler, val_reg);
                reg_free(compiler, obj_reg);
                return;
            }
        }
    }

    // Fallback path: compile object expression - use readonly version to avoid redundant MOVE
    XrExprDesc obj_expr = xr_compile_expr(ctx, compiler, node->object);
    int obj_reg = xexpr_to_anyreg_readonly(ctx, compiler, &obj_expr);

    // Compile value expression
    XrExprDesc val_expr = xr_compile_expr(ctx, compiler, node->value);
    int value_reg = xexpr_to_anyreg(ctx, compiler, &val_expr);

    // Use SETPROP (symbol lookup via per-function symbol table)
    int global_sym = xr_symbol_register_in_table(
        (XrSymbolTable *) xr_isolate_get_symbol_table(ctx->X), node->member);
    int local_sym = emitter_add_symbol(compiler->emitter, global_sym);
    xemit_setprop(compiler->emitter, obj_reg, local_sym, value_reg);

    // Free registers (consistent with other optimization paths)
    reg_free(compiler, value_reg);
    reg_free(compiler, obj_reg);
}
