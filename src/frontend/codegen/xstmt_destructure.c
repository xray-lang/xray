/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xstmt_destructure.c - Destructuring compilation implementation
 *
 * KEY CONCEPT:
 *   Flat (non-nested) destructuring only:
 *     - Array: let [a, b, c] = expr   →  a=expr[0], b=expr[1], c=expr[2]
 *     - Object: let {x, y} = expr     →  x=expr.x, y=expr.y
 *   No nesting, no rest (...), no Map destructuring.
 */

#include "xstmt.h"
#include "../../base/xchecks.h"
#include "../../runtime/xisolate_api.h"
#include "xcompiler.h"
#include "xcompiler_context.h"
#include "xcompiler_scope.h"
#include "xemit.h"
#include "xexpr.h"
#include "xexpr_desc.h"
#include "../../runtime/symbol/xsymbol_table.h"
#include "../../runtime/value/xtype.h"
#include <stdio.h>
#include <string.h>

// Forward declarations
static void compile_array_destructure(XrCompilerContext *ctx, XrCompiler *compiler, XrDestructurePattern *pattern, int source_reg, bool is_const, XrType *elem_type);
static void compile_object_destructure(XrCompilerContext *ctx, XrCompiler *compiler, XrDestructurePattern *pattern, int source_reg, bool is_const, XrType *init_type);

/*
 * Assign value from source_reg[index] to a named variable.
 * Used by array destructuring: let a = arr[index]
 */
static void compile_array_elem_to_var(XrCompilerContext *ctx, XrCompiler *compiler,
                                       const char *name, int source_reg, int index,
                                       bool is_const, XrType *elem_type) {
    (void)is_const;
    XrString *name_str = xr_compile_time_intern(ctx->X, name, strlen(name));

    int index_reg = reg_alloc(ctx, compiler);
    xemit_loadi(compiler->emitter, index_reg, index);

    int value_reg = reg_alloc(ctx, compiler);
    xemit_index_get(compiler->emitter, value_reg, source_reg, index_reg);
    reg_free(compiler, index_reg);

    if (compiler->scope_depth == 0) {
        int shared_index = shared_get_or_add(ctx, compiler, name_str);
        xemit_setshared(compiler->emitter, value_reg, shared_index);
        reg_free(compiler, value_reg);
    } else {
        XrLocalInfo *local = scope_define_local(ctx, compiler, name_str);
        if (elem_type) local_set_compile_type(local, elem_type);
        if (value_reg != local->reg) {
            emit_move(compiler->emitter, local->reg, value_reg);
        }
        reg_free(compiler, value_reg);
    }
}

/*
 * Assign value from source_reg.field to a named variable.
 * Used by object destructuring: let x = obj.x
 */
static void compile_object_field_to_var(XrCompilerContext *ctx, XrCompiler *compiler,
                                         const char *field, int source_reg, bool is_const,
                                         XrType *field_type) {
    (void)is_const;
    XrString *name_str = xr_compile_time_intern(ctx->X, field, strlen(field));
    int target_reg;

    if (compiler->scope_depth == 0) {
        target_reg = reg_alloc(ctx, compiler);
    } else {
        XrLocalInfo *local = scope_define_local(ctx, compiler, name_str);
        if (field_type) local_set_compile_type(local, field_type);
        target_reg = local->reg;
    }

    int global_sym = xr_symbol_register_in_table(
        (XrSymbolTable*)xr_isolate_get_symbol_table(ctx->X), field);
    int local_sym = emitter_add_symbol(compiler->emitter, global_sym);
    xemit_getprop(compiler->emitter, target_reg, source_reg, local_sym);

    if (compiler->scope_depth == 0) {
        int shared_index = shared_get_or_add(ctx, compiler, name_str);
        xemit_setshared(compiler->emitter, target_reg, shared_index);
        reg_free(compiler, target_reg);
    }
}

/*
 * Flat array destructure: let [a, b, c] = arr
 *   → a=arr[0], b=arr[1], c=arr[2]
 */
static void compile_array_destructure(XrCompilerContext *ctx, XrCompiler *compiler,
                                       XrDestructurePattern *pattern, int source_reg,
                                       bool is_const, XrType *elem_type) {
    int index = 0;
    for (int i = 0; i < pattern->as.array.element_count; i++) {
        XrDestructurePattern *elem = pattern->as.array.elements[i];

        if (elem->type == PATTERN_SKIP) {
            index++;
            continue;
        }

        if (elem->type == PATTERN_IDENTIFIER) {
            compile_array_elem_to_var(ctx, compiler,
                elem->as.identifier.name, source_reg, index, is_const, elem_type);
        }

        index++;
    }
}

/*
 * Flat object destructure: let {name, age} = person
 *   → name=person.name, age=person.age
 */
static void compile_object_destructure(XrCompilerContext *ctx, XrCompiler *compiler,
                                        XrDestructurePattern *pattern, int source_reg,
                                        bool is_const, XrType *init_type) {
    for (int i = 0; i < pattern->as.object.field_count; i++) {
        const char *field = pattern->as.object.field_names[i];
        XrDestructurePattern *vp = pattern->as.object.patterns[i];

        if (vp->type == PATTERN_IDENTIFIER) {
            XrType *field_type = (init_type && field)
                ? xr_type_object_get_field(init_type, field) : NULL;
            compile_object_field_to_var(ctx, compiler, field, source_reg, is_const,
                                        field_type);
        }
    }
}

/*
 * Compile destructure declaration: let [a, b] = arr  or  const {x, y} = obj
 */
void compile_destructure_decl(XrCompilerContext *ctx, XrCompiler *compiler, DestructureDeclNode *node) {
    XR_DCHECK(ctx != NULL, "compile_destructure_decl: NULL ctx");
    XR_DCHECK(compiler != NULL, "compile_destructure_decl: NULL compiler");
    XR_DCHECK(node != NULL, "compile_destructure_decl: NULL node");
    XrExprDesc expr = xr_compile_expr(ctx, compiler, node->initializer);
    int source_reg = xexpr_to_anyreg(ctx, compiler, &expr);

    // Infer initializer type for compile_type propagation
    XrType *init_type = node->initializer
        ? get_expr_type(ctx, compiler, node->initializer) : NULL;

    if (node->pattern->type == PATTERN_ARRAY) {
        XrType *elem_type = (init_type && XR_TYPE_IS_ARRAY(init_type))
            ? init_type->container.element_type : NULL;
        compile_array_destructure(ctx, compiler, node->pattern, source_reg,
                                   node->is_const, elem_type);
    } else if (node->pattern->type == PATTERN_OBJECT) {
        compile_object_destructure(ctx, compiler, node->pattern, source_reg,
                                    node->is_const, init_type);
    }

    reg_free(compiler, source_reg);
}

/*
 * Compile destructuring assignment: [a, b] = arr  or  {x, y} = obj
 * Variables already exist, no need to define.
 */
void compile_destructure_assign(XrCompilerContext *ctx, XrCompiler *compiler, DestructureAssignNode *node) {
    XR_DCHECK(ctx != NULL, "compile_destructure_assign: NULL ctx");
    XR_DCHECK(compiler != NULL, "compile_destructure_assign: NULL compiler");
    XR_DCHECK(node != NULL, "compile_destructure_assign: NULL node");
    XrExprDesc expr = xr_compile_expr(ctx, compiler, node->value);
    int source_reg = xexpr_to_anyreg(ctx, compiler, &expr);

    // Assignment: variables already exist, no compile_type propagation needed
    if (node->pattern->type == PATTERN_ARRAY) {
        compile_array_destructure(ctx, compiler, node->pattern, source_reg, false, NULL);
    } else if (node->pattern->type == PATTERN_OBJECT) {
        compile_object_destructure(ctx, compiler, node->pattern, source_reg, false, NULL);
    }

    reg_free(compiler, source_reg);
}

/*
 * Compile multi-value declaration
 * let a, b = foo()
 * 
 * Assigns multiple return values from initialization expression to multiple variables.
 * 
 * Implementation strategy:
 * 1. Compile initialization expression (usually function call)
 * 2. Function returns multiple values to consecutive registers
 * 3. Allocate local variable slot for each variable
 * 4. Copy return values to variable slots (or use directly)
 */
void compile_multi_var_decl(XrCompilerContext *ctx, XrCompiler *compiler, MultiVarDeclNode *node) {
    int name_count = node->name_count;
    int value_count = node->value_count;
    
    // No initialization: let a, b, c
    if (value_count == 0) {
        for (int i = 0; i < name_count; i++) {
            const char *name = node->names[i];
            XrString *name_str = xr_compile_time_intern(ctx->X, name, strlen(name));
            XrLocalInfo *local = scope_define_local(ctx, compiler, name_str);
            local->is_const = node->is_const;
            // Initialize to null
            xemit_loadnull(compiler->emitter, local->reg);
        }
        return;
    }
    
    // Single value (function call): let a, b = foo()
    if (value_count == 1 && node->values[0]->type == AST_CALL_EXPR) {
        int pc_before = PROTO_CODE_COUNT(compiler->proto);
        XrExprDesc expr = xr_compile_expr(ctx, compiler, node->values[0]);
        int base_reg = xexpr_to_anyreg(ctx, compiler, &expr);
        
        // Post-patch: set OP_CALL/OP_CALL_STATIC C parameter to actual nresults.
        // compile_call_internal hardcodes C=1; we fix it here where we know name_count.
        if (name_count > 1) {
            int code_count = PROTO_CODE_COUNT(compiler->proto);
            for (int pc = code_count - 1; pc >= pc_before; pc--) {
                XrInstruction inst = PROTO_CODE(compiler->proto, pc);
                OpCode op = GET_OPCODE(inst);
                if (op == OP_CALL || op == OP_CALL_STATIC) {
                    int _a = GETARG_A(inst);
                    int _b = GETARG_B(inst);
                    PROTO_SET_CODE(compiler->proto, pc,
                                   CREATE_ABC(op, _a, _b, name_count));
                    break;
                }
            }
        }
        
        // Infer return type for compile_type propagation
        XrType *ret_type = get_expr_type(ctx, compiler, node->values[0]);
        
        // Define all locals first (allocates registers)
        XrLocalInfo **locals = (XrLocalInfo**)xr_malloc(sizeof(XrLocalInfo*) * name_count);
        for (int i = 0; i < name_count; i++) {
            const char *name = node->names[i];
            XrString *name_str = xr_compile_time_intern(ctx->X, name, strlen(name));
            locals[i] = scope_define_local(ctx, compiler, name_str);
            locals[i]->is_const = node->is_const;
            // Propagate type from tuple elements or single return type
            XrType *var_type = NULL;
            if (ret_type && ret_type->kind == XR_KIND_TUPLE) {
                var_type = xr_type_tuple_get(ret_type, i);
            } else if (ret_type && i == 0) {
                var_type = ret_type;
            }
            if (var_type) local_set_compile_type(locals[i], var_type);
        }
        
        // Copy in reverse order to avoid clobbering return values
        for (int i = name_count - 1; i >= 0; i--) {
            int source_reg = base_reg + i;
            if (source_reg != locals[i]->reg) {
                emit_move(compiler->emitter, locals[i]->reg, source_reg);
            }
        }
        xr_free(locals);
        reg_free(compiler, base_reg);
        return;
    }
    
    // Multiple values: let a, b = 1, 2 - count must match
    if (value_count != name_count) {
        xr_compiler_error(ctx, compiler, 
            "multi-value declaration count mismatch: left %d, right %d", 
            name_count, value_count);
        return;
    }
    
    // First calculate all right-side values to temporary registers
    int *temp_regs = (int*)xr_malloc(sizeof(int) * value_count);
    for (int i = 0; i < value_count; i++) {
        XrExprDesc expr = xr_compile_expr(ctx, compiler, node->values[i]);
        temp_regs[i] = xexpr_to_anyreg(ctx, compiler, &expr);
    }
    
    // Define variables and assign values
    for (int i = 0; i < name_count; i++) {
        const char *name = node->names[i];
        XrString *name_str = xr_compile_time_intern(ctx->X, name, strlen(name));
        XrLocalInfo *local = scope_define_local(ctx, compiler, name_str);
        local->is_const = node->is_const;
        // Propagate type from each value expression
        XrType *val_type = get_expr_type(ctx, compiler, node->values[i]);
        if (val_type) local_set_compile_type(local, val_type);
        
        if (temp_regs[i] != local->reg) {
            emit_move(compiler->emitter, local->reg, temp_regs[i]);
        }
    }
    
    // Free temporary registers (reverse order)
    for (int i = value_count - 1; i >= 0; i--) {
        reg_free(compiler, temp_regs[i]);
    }
    xr_free(temp_regs);
}

/*
 * Compile multi-value assignment
 * a, b = b, a
 * 
 * Key: must first calculate all right-side values to temporary registers, then assign to left-side variables
 * This way can correctly handle variable swap a, b = b, a
 */
void compile_multi_assign(XrCompilerContext *ctx, XrCompiler *compiler, MultiAssignNode *node) {
    int value_count = node->value_count;
    int target_count = node->target_count;
    
    // Check count match: if right side is not single function call, left and right count must match
    bool is_single_call = (value_count == 1 && 
                           node->values[0]->type == AST_CALL_EXPR);
    
    if (!is_single_call && value_count != target_count) {
        xr_compiler_error(ctx, compiler, 
            "multi-value assignment count mismatch: left %d, right %d", 
            target_count, value_count);
        return;
    }
    
    // Step 1: Calculate all right-side values to temporary registers
    int pc_before = PROTO_CODE_COUNT(compiler->proto);
    int *temp_regs = (int*)xr_malloc(sizeof(int) * value_count);
    for (int i = 0; i < value_count; i++) {
        XrExprDesc expr = xr_compile_expr(ctx, compiler, node->values[i]);
        temp_regs[i] = xexpr_to_anyreg(ctx, compiler, &expr);
    }
    
    // Post-patch: single call producing multiple results → fix OP_CALL C parameter
    if (is_single_call && target_count > 1) {
        int code_count = PROTO_CODE_COUNT(compiler->proto);
        for (int pc = code_count - 1; pc >= pc_before; pc--) {
            XrInstruction inst = PROTO_CODE(compiler->proto, pc);
            OpCode op = GET_OPCODE(inst);
            if (op == OP_CALL || op == OP_CALL_STATIC) {
                int _a = GETARG_A(inst);
                int _b = GETARG_B(inst);
                PROTO_SET_CODE(compiler->proto, pc,
                               CREATE_ABC(op, _a, _b, target_count));
                break;
            }
        }
    }
    
    // Step 2: Assign temporary register values to left-side variables
    for (int i = 0; i < target_count; i++) {
        AstNode *target = node->targets[i];
        int source_reg = (i < value_count) ? temp_regs[i] : -1;
        
        if (source_reg < 0) {
            // Right-side values not enough, fill with null
            int nil_reg = reg_alloc(ctx, compiler);
            xemit_loadnull(compiler->emitter, nil_reg);
            source_reg = nil_reg;
        }
        
        // Generate assignment instruction based on target type
        if (target->type == AST_VARIABLE) {
            // Simple variable assignment
            const char *name = target->as.variable.name;
            XrString *name_str = xr_compile_time_intern(ctx->X, name, strlen(name));
            
            // Find local variable
            int local_reg = scope_resolve_local(compiler, name_str);
            if (local_reg >= 0) {
                // Local variable
                if (source_reg != local_reg) {
                    emit_move(compiler->emitter, local_reg, source_reg);
                }
            } else {
                // Top-level variable
                int shared_index = shared_get_or_add(ctx, compiler, name_str);
                xemit_setshared(compiler->emitter, source_reg, shared_index);
            }
        } else {
            // Other types of assignment targets (like member access) not yet supported
            xr_compiler_error(ctx, compiler, "multi-value assignment target must be variable");
        }
    }
    
    // Step 3: Free temporary registers (reverse order)
    for (int i = value_count - 1; i >= 0; i--) {
        reg_free(compiler, temp_regs[i]);
    }
    xr_free(temp_regs);
}
