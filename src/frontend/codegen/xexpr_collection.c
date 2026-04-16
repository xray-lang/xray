/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xexpr_collection.c - Collection expression compilation
 *
 * Handles collection-related expressions:
 * - Array literals [1, 2, 3]
 * - Map literals [key: value]
 * - Index access a[b]
 */

#include "xexpr.h"
#include "../../base/xchecks.h"
#include "xcompiler.h"
#include "xcompiler_context.h"
#include "xemit.h"
#include "xregalloc.h"
#include "../../runtime/symbol/xsymbol_table.h"
#include "../../runtime/object/xshape.h"
#include "../../runtime/object/xshape_cache.h"
#include "../../base/xmalloc.h"
#include "xexpr_desc.h"
#include "../../runtime/value/xtype.h"
#include "../analyzer/xanalyzer.h"
#include <stdio.h>
#include <string.h> // strlen

// Detect typed array element type from compile-time type info.
// Returns non-NULL XrType* for Array<int>/Array<float>, NULL otherwise.
static XrType *get_typed_array_elem_type(XrCompilerContext *ctx, AstNode *array_node) {
    if (!ctx->analyzer || !array_node) return NULL;
    XrType *arr_type = xa_analyzer_infer_expr_type(ctx->analyzer, array_node);
    if (!arr_type || arr_type->kind != XR_KIND_ARRAY) return NULL;
    XrType *elem = arr_type->container.element_type;
    if (!elem) return NULL;
    if (elem->kind == XR_KIND_INT || elem->kind == XR_KIND_FLOAT) return elem;
    return NULL;
}

// ========== Array Literals ==========

/*
 * Internal implementation: compile array literal (returns register)
 */
static int compile_array_literal_internal(XrCompilerContext *ctx, XrCompiler *compiler, ArrayLiteralNode *node) {
    XR_DCHECK(ctx != NULL, "compile_array_literal: NULL ctx");
    XR_DCHECK(compiler != NULL, "compile_array_literal: NULL compiler");
    // Allocate target register
    int array_reg = reg_alloc(ctx, compiler);
    
    // Create array: C = (elem_tid << 2) | storage_mode
    int c_field = ((int)ctx->current_elem_tid << 2) | ctx->current_storage_mode;
    emit_abc(compiler->emitter, OP_NEWARRAY, array_reg, node->count, c_field);
    
    // Use SETLIST to batch set elements
    if (node->count > 0) {
        // Compile elements directly to consecutive registers
        int first_elem_reg = array_reg + 1;
        compile_args_to_base(ctx, compiler, node->elements, node->count, first_elem_reg);
        
        // SETLIST: batch set array elements
        emit_abc(compiler->emitter, OP_ARRAY_INIT, array_reg, node->count, 0);
        
        // After operation, set freereg = base + 1
        xreg_set_freereg(compiler->regalloc, array_reg + 1);
    }
    
    return array_reg;
}

/*
 * Compile array literal (returns XrExprDesc)
 */
XrExprDesc compile_array_literal(XrCompilerContext *ctx, XrCompiler *compiler, ArrayLiteralNode *node) {
    XR_DCHECK(ctx != NULL, "compile_array_literal: NULL ctx");
    XR_DCHECK(compiler != NULL, "compile_array_literal: NULL compiler");
    XrExprDesc e = {0};
    int reg = compile_array_literal_internal(ctx, compiler, node);
    xexpr_init(&e, XEXPR_TEMP, reg);
    return e;
}


// ========== Map Literals ==========

/*
 * Internal implementation: compile Map literal (returns register)
 */
static int compile_map_literal_internal(XrCompilerContext *ctx, XrCompiler *compiler, MapLiteralNode *node) {
    // LIFO mode: allocate and protect target register
    int map_reg = xreg_alloc_temp(compiler->regalloc);
    int map_protect_id = xreg_protect_begin(compiler->regalloc, map_reg, 1, "map_literal");
    
    // Create empty Map (use dedicated OP_NEWMAP instruction)
    // OP_NEWMAP A B C: R[A] = #{}, B=capacity hint, C=(key_kind<<7)|(value_tid<<2)|flags
    int key_kind = (ctx->current_key_tid == XR_TID_STRING) ? 1 : (ctx->current_key_tid == XR_TID_INT) ? 2 : 0;
    int c_field = (key_kind << 7) | (((int)ctx->current_elem_tid & 0x1F) << 2) | ctx->current_storage_mode;
    emit_abc(compiler->emitter, OP_NEWMAP, map_reg, node->count, c_field);
    
    // Set each key-value pair (map_reg already auto-protected)
    if (node->count > 0) {
        
        for (int i = 0; i < node->count; i++) {
            // Compile value expression (compile value first, since key might be constant)
            XrExprDesc value_expr = xr_compile_expr(ctx, compiler, node->values[i]);
            // Optimization: value readonly, directly reuse source register, avoid MOVE
            int value_reg = xexpr_to_anyreg_readonly(ctx, compiler, &value_expr);
            
            // Optimization: constant string key uses OP_MAP_SETK
            if (node->keys[i]->type == AST_LITERAL_STRING) {
                LiteralNode *lit = (LiteralNode *)&node->keys[i]->as;
                const char *str_val = lit->raw_value.string_val;
                // Use compile-time interned string, same content shares same object, Map key comparison can use pointer comparison
                XrString *key_str = xr_compile_time_intern(ctx->X, str_val, strlen(str_val));
                int key_const = xr_vm_proto_add_constant(compiler->proto, xr_string_value(key_str));
                
                // OP_MAP_SETK A B C: R[A][K[B]] = R[C]
                emit_abc(compiler->emitter, OP_MAP_SETK, map_reg, key_const, value_reg);
            } else {
                // General path: compile key expression
                XrExprDesc key_expr = xr_compile_expr(ctx, compiler, node->keys[i]);
                // Optimization: key readonly
                int key_reg = xexpr_to_anyreg_readonly(ctx, compiler, &key_expr);
                
                // INDEX_SET: map[key] = value
                emit_abc(compiler->emitter, OP_INDEX_SET, map_reg, key_reg, value_reg);
                xexpr_free(compiler, &key_expr);
            }
            
            // Reclaim value temporary register (in readonly mode might be LOCAL, won't be freed)
            xexpr_free(compiler, &value_expr);
        }
    }
    
    // Release protection (but keep register value, as return value)
    xreg_protect_end(compiler->regalloc, map_protect_id);
    
    return map_reg;
}

XrExprDesc compile_map_literal(XrCompilerContext *ctx, XrCompiler *compiler, MapLiteralNode *node) {
    XR_DCHECK(ctx != NULL, "compile_map_literal: NULL ctx");
    XR_DCHECK(compiler != NULL, "compile_map_literal: NULL compiler");
    XrExprDesc e = {0};
    int reg = compile_map_literal_internal(ctx, compiler, node);
    xexpr_init(&e, XEXPR_TEMP, reg);
    return e;
}


// ========== Index Access ==========

/*
 * Compile index access expression
 *
 * Optimization: if index is string constant, use OP_MAP_GETK instead of LOADK + OP_INDEX_GET
 * This reduces one instruction, improving Map access performance
 */
XrExprDesc compile_index_get(XrCompilerContext *ctx, XrCompiler *compiler, IndexGetNode *node) {
    XR_DCHECK(ctx != NULL, "compile_index_get: NULL ctx");
    XR_DCHECK(compiler != NULL, "compile_index_get: NULL compiler");
    XrExprDesc e = {0};
    xexpr_init(&e, XEXPR_VOID, -1);
    
    // Compile array/Map expression - use readonly version to avoid redundant MOVE
    XrExprDesc array_expr = xr_compile_expr(ctx, compiler, node->array);
    int array_reg = xexpr_to_anyreg_readonly(ctx, compiler, &array_expr);
    
    // Optimization: constant string key uses OP_MAP_GETK
    if (node->index->type == AST_LITERAL_STRING) {
        LiteralNode *lit = (LiteralNode *)&node->index->as;
        const char *str_val = lit->raw_value.string_val;
        // Use compile-time interned string, shares same object with Map key
        XrString *key_str = xr_compile_time_intern(ctx->X, str_val, strlen(str_val));
        int key_const = xr_vm_proto_add_constant(compiler->proto, xr_string_value(key_str));
        
        // Allocate result register, avoid overwriting source register
        int result_reg = reg_alloc(ctx, compiler);
        
        // OP_MAP_GETK A B C: R[A] = R[B][K[C]]
        emit_abc(compiler->emitter, OP_MAP_GETK, result_reg, array_reg, key_const);
        reg_free(compiler, array_reg);
        
        // Return TEMP: already has determined register
        e.kind = XEXPR_TEMP;
        e.reg = result_reg;
        return e;
    }
    
    // Optimization: integer literal index uses OP_ARRAY_GETC (e.g. arr[0], arr[1])
    if (node->index->type == AST_LITERAL_INT) {
        LiteralNode *lit = (LiteralNode *)&node->index->as;
        int64_t idx = lit->raw_value.int_val;
        
        // C parameter only 8 bits (0-255), check range
        if (idx >= 0 && idx <= 255) {
            XrType *elem_type = get_typed_array_elem_type(ctx, node->array);
            if (elem_type) {
                // Typed array: use OP_TARRAY_GETC, output raw I64/F64
                int pc = emit_abc(compiler->emitter, OP_TARRAY_GETC, 0, array_reg, (int)idx);
                reg_free(compiler, array_reg);
                e.kind = XEXPR_RELOC;
                e.u.pc = pc;
                e.compile_type = elem_type;
                e.is_raw = true;
                return e;
            }
            // OP_ARRAY_GETC A B C: R[A] = R[B][C]
            int pc = emit_abc(compiler->emitter, OP_ARRAY_GETC, 0, array_reg, (int)idx);
            reg_free(compiler, array_reg);
            
            e.kind = XEXPR_RELOC;
            e.u.pc = pc;
            return e;
        }
    }
    
    // General path: compile index expression - use readonly version to avoid redundant MOVE
    XrExprDesc index_expr = xr_compile_expr(ctx, compiler, node->index);
    int index_reg = xexpr_to_anyreg_readonly(ctx, compiler, &index_expr);
    
    // Typed array fast path: raw in, raw out (no BOX/UNBOX)
    {
        XrType *elem_type = get_typed_array_elem_type(ctx, node->array);
        if (elem_type) {
            int result_reg = reg_alloc(ctx, compiler);
            emit_abc(compiler->emitter, OP_TARRAY_GET, result_reg, array_reg, index_reg);
            reg_free(compiler, index_reg);
            reg_free(compiler, array_reg);
            e.kind = XEXPR_TEMP;
            e.reg = result_reg;
            e.compile_type = elem_type;
            e.is_raw = true;
            return e;
        }
    }
    
    // BOX if typed (INDEX_GET expects tagged values)
    index_reg = xexpr_ensure_boxed(ctx, compiler, &index_expr, index_reg);
    
    /*
     * Fix: pre-allocate result register, avoid conflict with index_reg or array_reg
     * 
     * Problem scenario: a[i] * b[i]
     * If i is LOCAL in R[7], INDEX_GET's RELOC writeback might store result to R[7]
     * This would overwrite i's value, causing subsequent b[i] to use wrong index
     * 
     * Solution: pre-allocate target register, ensure it doesn't conflict with source operands
     */
    int result_reg = reg_alloc(ctx, compiler);
    
    /*
     * Bounds Check Elimination (BCE) optimization
     * 
     * Conditions:
     * 1. Currently in BCE loop (bce_loop_var != NULL)
     * 2. Index expression is loop variable (variable name matches)
     * 3. Use ARRAY_GET_NOCHECK to skip bounds check
     * 
     * Scenario: for (let i = 0; i < n; i++) { sum = arr[i] + ... }
     * If arr.length >= n, then arr[i] is safe
     */
    bool use_nocheck = false;
    if (compiler->bce_loop_var != NULL && node->index->type == AST_VARIABLE) {
        const char *idx_var_name = node->index->as.variable.name;
        if (strcmp(idx_var_name, compiler->bce_loop_var) == 0) {
            // Index is loop variable, can eliminate bounds check
            use_nocheck = true;
        }
    }
    
    // Generate INDEX_GET or ARRAY_GET_NOCHECK, target register already determined
    if (use_nocheck) {
        emit_abc(compiler->emitter, OP_ARRAY_GET_NOCHECK, result_reg, array_reg, index_reg);
    } else {
        emit_abc(compiler->emitter, OP_INDEX_GET, result_reg, array_reg, index_reg);
    }
    
    // Free temporary registers (if they are temporary)
    reg_free(compiler, index_reg);
    reg_free(compiler, array_reg);
    
    // Return TEMP: target register already determined
    e.kind = XEXPR_TEMP;
    e.reg = result_reg;
    return e;
}


// ========== Slice Expression ==========

/*
 * Compile slice expression: source[start:end]
 * 
 * Instruction format: OP_SLICE A B C
 *   R[A] = R[B][R[C]:R[C+1]]
 *   - R[B]: source object (Array/String/Bytes)
 *   - R[C]: start index (-1 means omitted, use default value 0)
 *   - R[C+1]: end index (-1 means omitted, use default value length)
 */
XrExprDesc compile_slice_expr(XrCompilerContext *ctx, XrCompiler *compiler, SliceExprNode *node) {
    XR_DCHECK(ctx != NULL, "compile_slice_expr: NULL ctx");
    XR_DCHECK(compiler != NULL, "compile_slice_expr: NULL compiler");
    XrExprDesc e = {0};
    xexpr_init(&e, XEXPR_VOID, -1);
    
    // Compile source object to register
    XrExprDesc source_e = xr_compile_expr(ctx, compiler, node->source);
    int source_reg = xexpr_to_anyreg(ctx, compiler, &source_e);
    
    // Reserve two consecutive registers for start and end (must be consecutive!)
    int start_reg = xreg_reserve(compiler->regalloc, 2);
    int end_reg = start_reg + 1;
    
    // Compile start index (if omitted, load 0 to start from beginning)
    if (node->start) {
        XrExprDesc start_e = xr_compile_expr(ctx, compiler, node->start);
        int temp_start = xexpr_to_anyreg(ctx, compiler, &start_e);
        if (temp_start != start_reg) {
            emit_abc(compiler->emitter, OP_MOVE, start_reg, temp_start, 0);
        }
    } else {
        // Omitted start, load 0 to start from beginning
        emit_abx(compiler->emitter, OP_LOADI, start_reg, 0);
    }
    
    // Compile end index (if omitted, load -1 to use default value length)
    if (node->end) {
        XrExprDesc end_e = xr_compile_expr(ctx, compiler, node->end);
        int temp_end = xexpr_to_anyreg(ctx, compiler, &end_e);
        if (temp_end != end_reg) {
            emit_abc(compiler->emitter, OP_MOVE, end_reg, temp_end, 0);
        }
    } else {
        // Omitted end, load -1 to use default value
        emit_abx(compiler->emitter, OP_LOADI, end_reg, -1);
    }
    
    // Result register
    int result_reg = xreg_alloc_temp(compiler->regalloc);
    
    // Generate OP_SLICE instruction
    emit_abc(compiler->emitter, OP_SLICE, result_reg, source_reg, start_reg);
    
    // Free temporary registers
    xreg_free_temp(compiler->regalloc, end_reg);
    xreg_free_temp(compiler->regalloc, start_reg);
    if (source_e.kind == XEXPR_TEMP) {
        xreg_free_temp(compiler->regalloc, source_reg);
    }
    
    e.kind = XEXPR_TEMP;
    e.reg = result_reg;
    return e;
}


// ========== Object Literals (using Json V2 optimization) ==========

/*
 * Internal implementation: compile object literal (returns register)
 * 
 * Optimization: using Json V2 (based on V8 Hidden Class)
 * - Zero-copy Shape transition
 * - Inline property value storage
 * - O(1) field access
 * - Field names use interned strings, pointer comparison O(1)
 * - Supports computed properties { [expr]: value }
 */
// Emit a single field initializer for object literal at the given shape index
static void emit_json_field_init(XrCompilerContext *ctx, XrCompiler *compiler,
                                  int obj_reg, uint32_t shape_idx, AstNode *val_node) {
    if (val_node->type == AST_LITERAL_NULL) {
        emit_abc(compiler->emitter, OP_JSON_INIT_N, obj_reg, shape_idx, 0);
    } else if (val_node->type == AST_LITERAL_INT) {
        int64_t int_val = val_node->as.literal.raw_value.int_val;
        if (int_val >= -128 && int_val <= 127) {
            emit_abc(compiler->emitter, OP_JSON_INIT_I, obj_reg, shape_idx,
                     (int)(int_val) & 0xFF);
        } else {
            int value_reg = xr_compile_expression(ctx, compiler, val_node);
            emit_abc(compiler->emitter, OP_JSON_INIT, obj_reg, shape_idx, value_reg);
            reg_free(compiler, value_reg);
        }
    } else {
        int value_reg = xr_compile_expression(ctx, compiler, val_node);
        emit_abc(compiler->emitter, OP_JSON_INIT, obj_reg, shape_idx, value_reg);
        reg_free(compiler, value_reg);
    }
}

static int compile_object_literal_internal(XrCompilerContext *ctx, XrCompiler *compiler, ObjectLiteralNode *node) {
    uint32_t field_count = node->count;
    
    // Check if has computed properties
    bool has_computed = node->computed != NULL;
    uint32_t static_count = 0;  // Static field count
    
    // Use interned strings to store static field names (pointer comparison O(1))
    XrString** interned_names = NULL;
    
    // Strict type alias: reorder literal fields to match type definition order.
    // This ensures Shape field indices match compile-time field_idx used by
    // OP_JSON_GET/SET and OP_TFIELD_GET/SET.
    XrType *target_type = ctx->current_object_type;
    ctx->current_object_type = NULL;  // Consume: prevent nested literals from inheriting
    bool needs_reorder = target_type
                         && !target_type->object.allow_extension
                         && target_type->object.field_count > 0
                         && target_type->object.field_names;
    
    // reorder_map[target_idx] = literal_idx, or -1 if missing
    int *reorder_map = NULL;
    
    if (needs_reorder) {
        int target_count = target_type->object.field_count;
        static_count = (uint32_t)target_count;
        
        reorder_map = (int*)xr_malloc(target_count * sizeof(int));
        interned_names = (XrString**)xr_malloc(target_count * sizeof(XrString*));
        
        for (int ti = 0; ti < target_count; ti++) {
            const char *target_name = target_type->object.field_names[ti];
            interned_names[ti] = xr_compile_time_intern(ctx->X, target_name, strlen(target_name));
            
            // Find matching literal field
            reorder_map[ti] = -1;
            for (uint32_t li = 0; li < field_count; li++) {
                if (has_computed && node->computed[li]) continue;
                if (node->keys[li]->type != AST_LITERAL_STRING) continue;
                const char *lit_name = node->keys[li]->as.literal.raw_value.string_val;
                if (strcmp(lit_name, target_name) == 0) {
                    reorder_map[ti] = (int)li;
                    break;
                }
            }
        }
    } else if (field_count > 0) {
        // Original path: use literal field order
        for (uint32_t i = 0; i < field_count; i++) {
            if (!has_computed || !node->computed[i]) {
                static_count++;
            }
        }
        
        if (static_count > 0) {
            interned_names = (XrString**)xr_malloc(static_count * sizeof(XrString*));
            uint32_t idx = 0;
            
            for (uint32_t i = 0; i < field_count; i++) {
                if (has_computed && node->computed[i]) {
                    continue;  // Skip computed properties
                }
                
                // key must be string literal
                if (node->keys[i]->type != AST_LITERAL_STRING) {
                    xr_compiler_error(ctx, compiler, 
                        "Object literal keys must be string literals");
                    if (interned_names) xr_free((void*)interned_names);
                    return reg_alloc(ctx, compiler);
                }
                
                const char *name = node->keys[i]->as.literal.raw_value.string_val;
                interned_names[idx++] = xr_compile_time_intern(ctx->X, name, strlen(name));
            }
        }
    }
    
    // Use Shape cache (static fields only)
    XrShape *shape = xr_shape_cache_get_or_create(
        ctx->X,
        ctx->shape_cache,
        interned_names,
        static_count
    );
    
    if (interned_names) {
        xr_free((void*)interned_names);
    }
    
    if (!shape) {
        if (reorder_map) xr_free(reorder_map);
        xr_compiler_error(ctx, compiler, "Failed to create shape");
        return reg_alloc(ctx, compiler);
    }
    
    // LIFO mode: allocate and protect target register
    int obj_reg = xreg_alloc_temp(compiler->regalloc);
    int obj_protect_id = xreg_protect_begin(compiler->regalloc, obj_reg, 1, "json_literal");
    
    // OP_NEWJSON: create Json object
    // Shape uses malloc allocation (not GC managed), stored as integer pointer
    // B = Shape constant index, C = storage mode
    int shape_const_idx = xr_vm_proto_add_constant(
        compiler->proto,
        xr_int((intptr_t)shape)
    );
    emit_abc(compiler->emitter, OP_NEWJSON, obj_reg, shape_const_idx, ctx->current_storage_mode);
    
    // Initialize fields
    if (needs_reorder) {
        // Emit in target type field order (reordered)
        for (uint32_t ti = 0; ti < static_count; ti++) {
            int li = reorder_map[ti];
            if (li < 0) {
                // Field not in literal, init to null
                emit_abc(compiler->emitter, OP_JSON_INIT_N, obj_reg, ti, 0);
            } else {
                emit_json_field_init(ctx, compiler, obj_reg, ti, node->values[li]);
            }
        }
        xr_free(reorder_map);
    } else if (field_count > 0) {
        // Original path: emit static fields in literal order, then computed
        // Static fields must be emitted BEFORE computed properties, because
        // INDEX_SET on computed keys converts Json to dictionary mode, which
        // makes JSON_INIT* (direct fields[] access) corrupt the object.
        uint32_t static_idx = 0;
        for (uint32_t i = 0; i < field_count; i++) {
            bool is_computed = has_computed && node->computed[i];
            if (is_computed) continue;
            
            emit_json_field_init(ctx, compiler, obj_reg, static_idx, node->values[i]);
            static_idx++;
        }
        
        // Pass 2: emit computed properties via INDEX_SET (may convert to dictionary)
        if (has_computed) {
            for (uint32_t i = 0; i < field_count; i++) {
                if (!node->computed[i]) continue;
                
                int value_reg = xr_compile_expression(ctx, compiler, node->values[i]);
                int key_reg = xr_compile_expression(ctx, compiler, node->keys[i]);
                emit_abc(compiler->emitter, OP_INDEX_SET, obj_reg, key_reg, value_reg);
                reg_free(compiler, key_reg);
                reg_free(compiler, value_reg);
            }
        }
    }
    
    // Release protection (but keep register value, as return value)
    xreg_protect_end(compiler->regalloc, obj_protect_id);
    
    return obj_reg;
}

XrExprDesc compile_object_literal(XrCompilerContext *ctx, XrCompiler *compiler, ObjectLiteralNode *node) {
    XrExprDesc e = {0};
    int reg = compile_object_literal_internal(ctx, compiler, node);
    xexpr_init(&e, XEXPR_TEMP, reg);
    return e;
}


// ========== Set Literals ==========

/*
 * Internal implementation: compile Set literal (returns register)
 */
static int compile_set_literal_internal(XrCompilerContext *ctx, XrCompiler *compiler, SetLiteralNode *node) {
    // Allocate target register
    int set_reg = reg_alloc(ctx, compiler);
    
    // Use dedicated OP_NEWSET instruction to create Set object
    // B = (elem_tid << 2) | storage_mode
    int b_field = ((int)ctx->current_elem_tid << 2) | ctx->current_storage_mode;
    emit_abc(compiler->emitter, OP_NEWSET, set_reg, b_field, 0);
    
    // Generate set.add(elem) call for each element
    if (node->count > 0) {
        // Get Symbol ID for 'add' method
        
        // Unified calling convention: R[base]=return value, R[base+1]=receiver, R[base+2]=arg1
        for (int i = 0; i < node->count; i++) {
            // LIFO mode: allocate consecutive registers and protect
            int base = xreg_get_freereg(compiler->regalloc);
            int call_protect_id = xreg_protect_begin(compiler->regalloc, base, 2, "set_add");
            xreg_reserve(compiler->regalloc, 2);
            
            // receiver (Set) to R[base+1]
            emit_move(compiler->emitter, base + 1, set_reg);
            xreg_set_freereg(compiler->regalloc, base + 2);
            
            // Compile element expression to R[base+2] (base and base+1 already auto-protected)
            XrExprDesc elem_expr = xr_compile_expr(ctx, compiler, node->elements[i]);
            xexpr_to_specific_reg(ctx, compiler, &elem_expr, base + 2);
            
            // End protection
            xreg_protect_end(compiler->regalloc, call_protect_id);
            
            // Generate method call: set.add(elem)
            int local_sym = emitter_add_symbol(compiler->emitter, SYMBOL_ADD);
            emit_abc(compiler->emitter, OP_INVOKE, base, local_sym, 1);
            
            // Reclaim registers
            xreg_set_freereg(compiler->regalloc, base + 1);
        }
    }
    
    return set_reg;
}

XrExprDesc compile_set_literal(XrCompilerContext *ctx, XrCompiler *compiler, SetLiteralNode *node) {
    XR_DCHECK(ctx != NULL, "compile_set_literal: NULL ctx");
    XR_DCHECK(compiler != NULL, "compile_set_literal: NULL compiler");
    XrExprDesc e = {0};
    int reg = compile_set_literal_internal(ctx, compiler, node);
    xexpr_init(&e, XEXPR_TEMP, reg);
    return e;
}


