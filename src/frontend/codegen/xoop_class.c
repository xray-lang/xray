/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xoop_class.c - Compile-time class building - generates class creation bytecode
 *
 * KEY CONCEPT:
 *   Compiler compiles class declarations into bytecode sequences.
 *   Runtime VM executes bytecode to create XrClass objects.
 *   Method closures are generated at compile-time, set to class at runtime.
 *   After finalize, class is completely immutable.
 */

#include "xoop.h"
#include "../../base/xchecks.h"
#include "xoop_class_descriptor_builder.h"  // ClassDescriptor MVP
#include "../../runtime/xisolate_api.h"
#include "../../runtime/xglobals_table.h"  // xr_globals_get
#include "xcompiler.h"
#include "xcompiler_context.h"
#include "xcompiler_class_registry.h"  // Compile-time optimization
#include "../../runtime/value/xstruct_layout.h"
#include "xregalloc.h"
#include "xemit.h"
#include "xstmt.h"
#include "xexpr.h"  // Expression compilation
#include "../../runtime/object/xstring.h"
#include "../../runtime/class/xclass.h"
#include "../../runtime/symbol/xsymbol_table.h"
#include "../../runtime/class/xclass_builder.h"
#include "../../runtime/class/xclass_descriptor.h"
#include "../../runtime/class/xmethod.h"
#include "../../runtime/value/xvalue.h"
#include "../parser/xparse.h" // xr_compile_type_to_string
#include <stdio.h>
#include <string.h>

// ========== Helper Functions ==========

// Check if constructor body contains an explicit super() call (top-level only)
static bool constructor_has_super_call(MethodDeclNode *method) {
    if (!method || !method->body) return false;
    AstNode *body = method->body;
    if (body->type != AST_BLOCK) return false;
    BlockNode *block = &body->as.block;
    for (int i = 0; i < block->count; i++) {
        AstNode *stmt = block->statements[i];
        if (!stmt) continue;
        if (stmt->type == AST_SUPER_CALL) return true;
        if (stmt->type == AST_EXPR_STMT && stmt->as.expr_stmt &&
            stmt->as.expr_stmt->type == AST_SUPER_CALL) return true;
    }
    return false;
}

/*
 * Compile single method and return its index
 */
static int compile_method_and_get_index(XrCompilerContext *ctx, XrCompiler *compiler, 
                                        MethodDeclNode *method, int *closure_reg) {
    XR_DCHECK(ctx != NULL, "compile_method: NULL ctx");
    XR_DCHECK(compiler != NULL, "compile_method: NULL compiler");
    XR_DCHECK(method != NULL, "compile_method: NULL method");
    // Check if operator method, set context
    const char *old_operator = ctx->current_operator;
    if (method->is_operator && method->name != NULL) {
        ctx->current_operator = method->name;  // Set current operator being compiled
    }
    
    // Compile method function
    XrCompiler method_compiler;
    xr_compiler_init(ctx, &method_compiler, FUNCTION_FUNCTION);
    method_compiler.enclosing = compiler;  // Set enclosing compiler, support upvalue capture
    
    // Set method name
    XrString *method_name_str = xr_compile_time_intern(ctx->X, method->name, strlen(method->name));
    method_compiler.proto->name = method_name_str;
    
    // Set return type (if specified)
    if (method->return_type != NULL) {
        const char *type_str = xr_compile_type_to_string(method->return_type);
        method_compiler.proto->return_type = type_str ? strdup(type_str) : NULL;
        method_compiler.declared_return_type = method->return_type;
    }
    
    // Set parameters (parameters use fixed registers 0, 1, 2...)
    if (method->is_static) {
        // Static method: param count = explicit params
        method_compiler.proto->numparams = method->param_count;
        
        // Directly add explicit params (registers 0, 1, 2...)
        for (int p = 0; p < method->param_count; p++) {
            XrString *param_name = xr_compile_time_intern(ctx->X, method->parameters[p], strlen(method->parameters[p]));
            scope_define_local_reg(ctx, &method_compiler, param_name, p);
        }
    } else {
        // Instance method: param count = this + explicit params
        method_compiler.proto->numparams = method->param_count + 1;
        
        // Param 0 is this (implicit, fixed in register 0)
        XrString *this_name = xr_compile_time_intern(ctx->X, "this", 4);
        scope_define_local_reg(ctx, &method_compiler, this_name, 0);
        
        // Add explicit params (registers 1, 2, 3...)
        for (int p = 0; p < method->param_count; p++) {
            XrString *param_name = xr_compile_time_intern(ctx->X, method->parameters[p], strlen(method->parameters[p]));
            scope_define_local_reg(ctx, &method_compiler, param_name, p + 1);
        }
    }
    
    // After params defined, freereg should equal local_end
    if (method_compiler.regalloc) {
        xreg_set_freereg(method_compiler.regalloc, xreg_get_local_end(method_compiler.regalloc));
    }
    
    // Compile method body
    if (method->is_abstract) {
        // Abstract method: generate error instruction
        EMIT_ABC(ctx, &method_compiler, OP_ABSTRACT_ERROR, 0, 0, 0);
        EMIT_ABC(ctx, &method_compiler, OP_RETURN, 0, 0, 0);
    } else if (method->body != NULL) {
        // Constructor: auto-insert super() and compile field defaults (before method body)
        if ((method->is_constructor || strcmp(method->name, XR_KEYWORD_CONSTRUCTOR) == 0) && 
            ctx->current_class_node != NULL) {
            ClassDeclNode *class_node = (ClassDeclNode*)ctx->current_class_node;
            
            // Auto-insert super() if: subclass constructor has no explicit super()
            // and parent has a constructor with 0 required params
            if (class_node->super_name && !constructor_has_super_call(method) &&
                ctx->class_registry) {
                ClassInfo *parent_ci = xr_class_registry_lookup(
                    ctx->class_registry, class_node->super_name);
                if (parent_ci && parent_ci->has_constructor &&
                    parent_ci->constructor_required_params == 0) {
                    // Emit: super.constructor() with 0 args
                    // Layout: R[base]=result, R[base+1]=this, no args
                    int base = xreg_get_freereg(method_compiler.regalloc);
                    xreg_set_freereg(method_compiler.regalloc, base + 2);
                    EMIT_ABC(ctx, &method_compiler, OP_MOVE, base + 1, 0, 0);
                    XrString *ctor_str = xr_compile_time_intern(
                        ctx->X, XR_KEYWORD_CONSTRUCTOR, strlen(XR_KEYWORD_CONSTRUCTOR));
                    int ctor_const = xr_vm_proto_add_constant(
                        method_compiler.proto, xr_string_value(ctor_str));
                    EMIT_ABC(ctx, &method_compiler, OP_SUPERINVOKE, base, ctor_const, 0);
                    xreg_set_freereg(method_compiler.regalloc,
                        xreg_get_local_end(method_compiler.regalloc));
                }
            }
            
            for (int i = 0; i < class_node->field_count; i++) {
                if (class_node->fields[i]->type != AST_FIELD_DECL) continue;
                FieldDeclNode *field = &class_node->fields[i]->as.field_decl;
                
                // Skip static fields and fields without initializer
                if (field->is_static || field->initializer == NULL) continue;
                
                // Skip simple literals (already handled in ClassDescriptor)
                AstNodeType init_type = field->initializer->type;
                if (init_type == AST_LITERAL_NULL ||
                    init_type == AST_LITERAL_TRUE ||
                    init_type == AST_LITERAL_FALSE ||
                    init_type == AST_LITERAL_INT ||
                    init_type == AST_LITERAL_FLOAT ||
                    init_type == AST_LITERAL_STRING) {
                    continue;
                }
                
                // Complex expression: compile initialization code
                // this.field = initializer
                int value_reg = xr_compile_expression(ctx, &method_compiler, field->initializer);
                
                // Find field index
                int field_idx = -1;
                if (ctx->current_class_desc) {
                    XrClassDescriptor *desc = ctx->current_class_desc;
                    for (uint32_t fi = 0; fi < desc->instance_field_count; fi++) {
                        if (strcmp(desc->instance_fields[fi].name, field->name) == 0) {
                            field_idx = (int)fi;
                            break;
                        }
                    }
                }
                
                if (field_idx >= 0) {
                    // Use optimized OP_SETFIELD instruction: R[A].fields[B] = R[C]
                    EMIT_ABC(ctx, &method_compiler, OP_SETFIELD, 0, field_idx, value_reg);
                } else {
                    // Fallback: use OP_SETPROP with per-function symbol table
                    int global_sym = xr_symbol_register_in_table((XrSymbolTable*)xr_isolate_get_symbol_table(ctx->X), field->name);
                    int local_sym = xr_proto_add_symbol(method_compiler.proto, global_sym);
                    EMIT_ABC(ctx, &method_compiler, OP_SETPROP, 0, local_sym, value_reg);
                }
                
                reg_free(&method_compiler, value_reg);
            }
            
            // Restore freereg
            xreg_set_freereg(method_compiler.regalloc, xreg_get_local_end(method_compiler.regalloc));
        }
        
        // Struct value semantics: copy struct-typed params at method entry
        // Skip for in/ref params (they pass by reference, no copy needed)
        for (int p = 0; p < method->param_count; p++) {
            uint8_t pmode = (method->param_passing_modes)
                            ? method->param_passing_modes[p] : XR_PARAM_VALUE;
            if (pmode != XR_PARAM_VALUE) continue;
            XrLocalInfo *plocal = compiler_get_local_by_name(
                &method_compiler, method->parameters[p]);
            if (!plocal) continue;
            bool is_vt = plocal->compile_type && plocal->compile_type->is_value_type;
            if (!is_vt && plocal->compile_type && ctx->analyzer) {
                const char *cn = NULL;
                if (plocal->compile_type->kind == XR_KIND_CLASS ||
                    plocal->compile_type->kind == XR_KIND_INSTANCE)
                    cn = plocal->compile_type->instance.class_name;
                if (cn) {
                    XaSymbol *cs = xa_analyzer_lookup(ctx->analyzer, cn);
                    if (cs && cs->kind == XA_SYM_CLASS) {
                        XaSymbolLinks *cl = xa_analyzer_get_links(ctx->analyzer, cs);
                        if (cl && cl->type && cl->type->is_value_type) is_vt = true;
                    }
                }
            }
            if (is_vt) {
                // Struct with layout -> OP_STRUCT_COPY
                bool used_sc = false;
                if (plocal->compile_type && ctx->class_registry) {
                    const char *cn2 = plocal->compile_type->instance.class_name;
                    if (cn2) {
                        ClassInfo *ci2 = xr_class_registry_lookup(ctx->class_registry, cn2);
                        if (ci2 && ci2->struct_layout) {
                            int sz2 = (8 + ci2->struct_layout->total_size + 15) & ~15;
                            int slot2 = method_compiler.struct_area_offset / 16;
                            method_compiler.struct_area_offset += sz2;
                            EMIT_ABC(ctx, &method_compiler, OP_STRUCT_COPY,
                                     plocal->reg, plocal->reg, slot2);
                            used_sc = true;
                        }
                    }
                }
                if (!used_sc)
                    EMIT_ABC(ctx, &method_compiler, OP_COPY,
                             plocal->reg, plocal->reg, 0);
            }
        }
        
        // Normal method: compile method body
        xr_compile_statement(ctx, &method_compiler, method->body);
        
        // Constructor auto-returns this
        if (method->is_constructor || strcmp(method->name, XR_KEYWORD_CONSTRUCTOR) == 0) {
            EMIT_ABC(ctx, &method_compiler, OP_RETURN, 0, 1, 0);
        }
    }
    
    // End method compilation
    XrProto *method_proto = xr_compiler_end(ctx, &method_compiler);
    if (method_proto == NULL) {
        ctx->current_operator = old_operator;  // Restore
        return -1;
    }
    
    // Add XrProto to current function's constant pool
    int proto_idx = xr_vm_proto_add_proto(compiler->proto, method_proto);
    
    // Create closure
    *closure_reg = reg_alloc(ctx, compiler);
    emit_ctx_sync_before_closure(ctx, compiler);
    EMIT_ABX(ctx, compiler, OP_CLOSURE, *closure_reg, proto_idx);
    
    // Restore context
    ctx->current_operator = old_operator;
    
    return proto_idx;  // Return proto index (for debugging)
}

// ========== ClassDescriptor Implementation ==========

/*
 * Compile class using ClassDescriptor
 */
static void compile_class_with_descriptor(
    XrCompilerContext *ctx,
    XrCompiler *compiler,
    ClassDeclNode *node
) {
    // Compile class
    
    // 1. Validate interface implementation (if any)
    if (node->interface_count > 0) {
        // Validate interface implementation
        
        for (int i = 0; i < node->interface_count; i++) {
            const char *iface_name = node->interfaces[i];
            
            // Find interface definition (check shared first, then globals)
            XrString *iface_name_str = xr_compile_time_intern(ctx->X, iface_name, strlen(iface_name));
            int iface_idx = shared_get_in_scope(ctx, compiler, iface_name_str);
            if (iface_idx < 0) {
                iface_idx = builtin_get(ctx, iface_name_str);
            }
            
            if (iface_idx == -1) {
                xr_compiler_error(ctx, compiler, 
                    "Interface '%s' not defined (class '%s' trying to implement)", iface_name, node->name);
                continue;
            }
            
            // Validate class implements all interface methods
            // Find interface object from constant pool
            XrClass *iface_obj = NULL;
            XrProto *proto = compiler->proto;
            for (int k = 0; k < DYNARRAY_COUNT(&proto->constants); k++) {
                XrValue kv = DYNARRAY_GET(&proto->constants, k, XrValue);
                if (xr_value_is_class(kv)) {
                    XrClass *cls = xr_value_to_class(kv);
                    if (cls->name && strcmp(cls->name, iface_name) == 0 &&
                        (cls->flags & XR_CLASS_INTERFACE)) {
                        iface_obj = cls;
                        break;
                    }
                }
            }
            
            if (iface_obj && iface_obj->abstract_method_count > 0) {
                XrSymbolTable *symtab = (XrSymbolTable*)xr_isolate_get_symbol_table(ctx->X);
                
                for (int m = 0; m < iface_obj->abstract_method_count; m++) {
                    int sym = iface_obj->abstract_methods[m];
                    const char *method_name = xr_symbol_get_name_in_table(symtab, sym);
                    if (!method_name) continue;
                    
                    // Get expected param count from interface method
                    int expected_params = -1;
                    for (int im = 0; im < iface_obj->method_count; im++) {
                        if (iface_obj->methods[im].symbol == sym) {
                            expected_params = iface_obj->methods[im].param_count;
                            break;
                        }
                    }
                    
                    // Check class AST has this method with matching param count
                    bool found = false;
                    for (int j = 0; j < node->method_count; j++) {
                        if (node->methods[j] && node->methods[j]->type == AST_METHOD_DECL) {
                            if (strcmp(node->methods[j]->as.method_decl.name, method_name) == 0) {
                                found = true;
                                if (expected_params >= 0) {
                                    int actual = node->methods[j]->as.method_decl.param_count;
                                    if (actual != expected_params) {
                                        xr_compiler_error(ctx, compiler,
                                            "class '%s' method '%s' has %d params, "
                                            "interface '%s' expects %d",
                                            node->name, method_name, actual,
                                            iface_name, expected_params);
                                    }
                                }
                                break;
                            }
                        }
                    }
                    if (!found) {
                        xr_compiler_error(ctx, compiler,
                            "class '%s' does not implement interface method '%s.%s'",
                            node->name, iface_name, method_name);
                    }
                }
            }
        }
    }
    
    // 2. Create ClassDescriptor
    XrClassDescriptor *desc = xoop_create_class_descriptor(ctx, compiler, node);
    if (!desc) {
        xr_compiler_error(ctx, compiler, 
            "Failed to create ClassDescriptor for class '%s'", node->name);
        return;
    }
    
    XrString *class_name_str = xr_compile_time_intern(ctx->X, node->name, strlen(node->name));
    bool is_inner_class = (compiler->scope_depth > 0);
    int class_global_idx = -1;
    
    if (!is_inner_class) {
        // Module/top-level: allocate shared variable for cross-scope access
        class_global_idx = shared_get_or_add(ctx, compiler, class_name_str);
        shared_set_const(ctx, class_global_idx, true);
        shared_set_type(ctx, class_global_idx, xr_type_new_class(node->name));
    }
    
    // Register class to class_registry (for compile-time type optimization)
    if (ctx->class_registry) {
        ClassInfo *class_info = xr_class_registry_register(ctx->class_registry, node->name);
        if (class_info) {
            // Copy all parent fields first (so instance_field_count reflects total)
            int parent_field_count = 0;
            if (node->super_name != NULL && node->super_module == NULL) {
                ClassInfo *parent_info = xr_class_registry_lookup(ctx->class_registry, node->super_name);
                if (parent_info) {
                    parent_field_count = parent_info->instance_field_count;
                    for (int i = 0; i < parent_info->instance_field_count; i++) {
                        xr_class_add_instance_field(class_info, 
                                                    parent_info->instance_fields[i].name,
                                                    parent_info->instance_fields[i].index);
                    }
                }
            }
            
            // Register own instance fields with correct offset and type
            for (uint32_t i = 0; i < desc->instance_field_count; i++) {
                const char *tn = desc->instance_fields[i].type_name;
                uint8_t slot_type = 0;  // XR_SLOT_ANY
                if (tn) {
                    if (strcmp(tn, "int") == 0 || strcmp(tn, "int64") == 0)
                        slot_type = 7;  // XR_SLOT_I64
                    else if (strcmp(tn, "float") == 0 || strcmp(tn, "float64") == 0)
                        slot_type = 10; // XR_SLOT_F64
                }
                xr_class_add_instance_field_typed(class_info, desc->instance_fields[i].name,
                                                  parent_field_count + (int)i, slot_type);
            }
            
            // Record constructor info for smart super() auto-insertion
            for (int i = 0; i < node->method_count; i++) {
                if (node->methods[i]->type != AST_METHOD_DECL) continue;
                MethodDeclNode *md = &node->methods[i]->as.method_decl;
                if (md->is_constructor) {
                    class_info->has_constructor = true;
                    int required = 0;
                    for (int p = 0; p < md->param_count; p++) {
                        if (!md->default_values || !md->default_values[p])
                            required++;
                    }
                    class_info->constructor_required_params = required;
                    break;
                }
            }
            
            // Copy struct_layout from analyzer (VALUE_TYPE only)
            if (ctx->is_compiling_struct && ctx->analyzer) {
                class_info->is_value_type = true;
                XaSymbol *xa_sym = xa_analyzer_lookup(ctx->analyzer, node->name);
                if (xa_sym) {
                    XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, xa_sym);
                    if (links && links->class_info && links->class_info->struct_layout) {
                        class_info->struct_layout = links->class_info->struct_layout;
                    }
                }
            }
            
            // Flatten parent methods into ClassInfo (matches runtime flattened layout)
            if (node->super_name != NULL && node->super_module == NULL) {
                ClassInfo *parent_info = xr_class_registry_lookup(ctx->class_registry, node->super_name);
                if (parent_info) {
                    for (int i = 0; i < parent_info->method_count; i++) {
                        xr_class_add_method(class_info,
                                            parent_info->methods[i].name,
                                            parent_info->methods[i].index);
                    }
                }
            }
        }
    }
    
    // 2.1. Record parent class global_index (compiler optimization: O(1) parent lookup)
    if (node->super_name != NULL) {
        if (node->super_module != NULL) {
            /*
             * Module member access form: extends module.Class
             * Cannot use global_index optimization, set parent via OP_INHERIT at runtime
             * Keep super_global_index = -1, also clear super_name to avoid name lookup
             */
            desc->super_name = NULL;
        } else {
            /*
             * Simple form: extends Class
             * Check if parent comes from import (local or upvalue)
             * If so, cannot use global_index optimization, handled by OP_INHERIT
             */
            XrString *super_name_str = xr_compile_time_intern(ctx->X, node->super_name, strlen(node->super_name));
            XrLocalInfo *local = compiler_get_local_by_name(compiler, node->super_name);
            int upvalue_idx = scope_resolve_upvalue(ctx, compiler, super_name_str);
            
            if (local || upvalue_idx >= 0) {
                /*
                 * Parent comes from imported module, don't use global_index optimization
                 * Clear super_name, let ClassFromDescriptor default inherit Object
                 * OP_INHERIT will correctly override parent
                 */
                desc->super_name = NULL;
                desc->super_global_index = -1;
            } else {
                // Parent defined in same module, rely on name lookup at runtime
                // (shared indices are local and need offset adjustment at runtime,
                //  but ClassFromDescriptor doesn't have access to proto->shared_offset)
                desc->super_global_index = -1;
            }
        }
    }
    
    // 2.5. Fill interface pointers
    if (desc->interface_count > 0) {
        for (uint32_t i = 0; i < desc->interface_count; i++) {
            const char *iface_name = desc->interfaces[i].interface_name;
            
            // Find interface object from constant pool
            // Traverse constant pool, find XrClass object with matching name
            XrProto *proto = compiler->proto;
            XrClass *iface_ptr = NULL;
            
            for (int k_idx = 0; k_idx < DYNARRAY_COUNT(&proto->constants); k_idx++) {
                XrValue k_val = DYNARRAY_GET(&proto->constants, k_idx, XrValue);
                if (xr_value_is_class(k_val)) {
                    XrClass *cls = xr_value_to_class(k_val);
                    if (cls->name && strcmp(cls->name, iface_name) == 0) {
                        iface_ptr = cls;
                        break;
                    }
                }
            }
            
            if (iface_ptr) {
                desc->interfaces[i].interface_ptr = iface_ptr;
            } else {
                fprintf(stderr, "[ClassDescriptor]   ERROR: Interface '%s' not found in constants pool\n", 
                        iface_name);
            }
        }
    }
    
    // For inner classes: pre-define local variable BEFORE compiling methods.
    // Static methods resolve the class name via upvalue from this local.
    // The register will be populated after CLASS_CREATE_FROM_DESCRIPTOR.
    XrLocalInfo *inner_class_local = NULL;
    if (is_inner_class) {
        inner_class_local = scope_define_local(ctx, compiler, class_name_str);
        inner_class_local->is_const = true;
    }
    
    // 3a. Pre-register all instance method names in ClassInfo before compiling bodies.
    // This allows this.method() inside a method body to resolve sibling methods
    // via OP_INVOKE_DIRECT even when the callee is declared later in the class.
    if (ctx->class_registry) {
        ClassInfo *ci = xr_class_registry_lookup(ctx->class_registry, node->name);
        if (ci) {
            uint32_t pre_idx = 0;
            (void)pre_idx;
            for (int j = 0; j < node->method_count; j++) {
                if (node->methods[j]->type != AST_METHOD_DECL) continue;
                MethodDeclNode *method = &node->methods[j]->as.method_decl;
                if (method->is_static) continue;
                
                int flat_idx = xr_class_find_method_index(ci, method->name);
                if (flat_idx < 0) {
                    // New method (not inherited): register at next available slot
                    xr_class_add_method(ci, method->name, ci->method_count);
                }
                pre_idx++;
            }
        }
    }
    
    // 3b. Compile instance methods and update closure_index in descriptor
    for (uint32_t i = 0; i < desc->instance_method_count; i++) {
        // Find corresponding method AST node
        uint32_t method_idx = 0;
        for (int j = 0; j < node->method_count; j++) {
            if (node->methods[j]->type != AST_METHOD_DECL) continue;
            MethodDeclNode *method = &node->methods[j]->as.method_decl;
            if (method->is_static) continue;
            
            if (method_idx == i) {
                // Compile method
                int closure_reg;
                // Set current class descriptor and node for optimization and field initialization
                XrClassDescriptor *saved_class_desc = ctx->current_class_desc;
                void *saved_class_node = ctx->current_class_node;
                ctx->current_class_desc = desc;
                ctx->current_class_node = node;
                int proto_idx = compile_method_and_get_index(ctx, compiler, method, &closure_reg);
                ctx->current_class_desc = saved_class_desc;  // Restore
                ctx->current_class_node = saved_class_node;
                if (proto_idx < 0) {
                    xr_compiler_error(ctx, compiler, 
                        "Failed to compile instance method '%s'", method->name);
                    xoop_free_class_descriptor(desc);
                    return;
                }
                
                // Store proto_idx, VM will get XrProto from subprotos at execution
                desc->instance_methods[i].closure_index = (uint32_t)proto_idx;
                
                // Register method in ClassInfo for OP_INVOKE_DIRECT optimization
                // xr_class_add_method handles override: if method name already exists
                // (inherited from parent), it updates the index in-place to keep the
                // parent's flattened slot. For new methods, we pass the current
                // method_count which gives the correct append position.
                if (ctx->class_registry && method->name) {
                    ClassInfo *ci = xr_class_registry_lookup(ctx->class_registry, node->name);
                    if (ci) {
                        int flat_idx = xr_class_find_method_index(ci, method->name);
                        if (flat_idx >= 0) {
                            // Override: keep parent slot, just update (index unchanged)
                            xr_class_add_method(ci, method->name, flat_idx);
                        } else {
                            // New method: append at current count position
                            xr_class_add_method(ci, method->name, ci->method_count);
                        }
                    }
                }
                
                reg_free(compiler, closure_reg);
                break;
            }
            method_idx++;
        }
    }
    
    // 3b. Check and compile static constructor (priority processing)
    MethodDeclNode *static_ctor = NULL;
    for (int j = 0; j < node->method_count; j++) {
        if (node->methods[j]->type != AST_METHOD_DECL) continue;
        MethodDeclNode *method = &node->methods[j]->as.method_decl;
        
        if (method->is_static_constructor) {
            static_ctor = method;
            break;
        }
    }
    
    // Compile static constructor
    if (static_ctor != NULL) {
        int closure_reg;
        int proto_idx = compile_method_and_get_index(ctx, compiler, static_ctor, &closure_reg);
        if (proto_idx < 0) {
            xr_compiler_error(ctx, compiler, 
                "Failed to compile static constructor for class '%s'", node->name);
            xoop_free_class_descriptor(desc);
            return;
        }
        
        // Store proto_idx to descriptor
        desc->clinit_proto_index = proto_idx;
        
        reg_free(compiler, closure_reg);
    } else {
        // No static constructor
        desc->clinit_proto_index = -1;
    }
    
    // 3c. Compile static methods and update closure_index in descriptor
    for (uint32_t i = 0; i < desc->static_method_count; i++) {
        // Find corresponding method AST node
        uint32_t method_idx = 0;
        for (int j = 0; j < node->method_count; j++) {
            if (node->methods[j]->type != AST_METHOD_DECL) continue;
            MethodDeclNode *method = &node->methods[j]->as.method_decl;
            if (!method->is_static || method->is_static_constructor) continue;  // Skip static constructor
            
            if (method_idx == i) {
                // Compile static method
                int closure_reg;
                int proto_idx = compile_method_and_get_index(ctx, compiler, method, &closure_reg);
                if (proto_idx < 0) {
                    xr_compiler_error(ctx, compiler, 
                        "Failed to compile static method '%s'", method->name);
                    xoop_free_class_descriptor(desc);
                    return;
                }
                
                // Store proto_idx
                desc->static_methods[i].closure_index = (uint32_t)proto_idx;
                
                reg_free(compiler, closure_reg);
                break;
            }
            method_idx++;
        }
    }
    
    // 4. Store ClassDescriptor in constant pool
    int desc_idx = xoop_add_descriptor_to_constant_pool(compiler->proto, desc);
    if (desc_idx < 0) {
        xr_compiler_error(ctx, compiler, 
            "Failed to add ClassDescriptor to constant pool");
        xoop_free_class_descriptor(desc);
        return;
    }
    
    // 5. Allocate class register
    int class_reg = reg_alloc(ctx, compiler);
    
    // 6. Generate single instruction
    EMIT_ABX(ctx, compiler, OP_CLASS_CREATE_FROM_DESCRIPTOR, class_reg, desc_idx);
    
    /*
     * 7. Store class definition
     * 
     * Module-level class definitions use local variables (better performance)
     * REPL mode: top-level class uses shared variable (persist across inputs)
     * Other non-module top-level: shared variable
     */
    bool is_module_level = (compiler->scope_depth == 0 && compiler->type == FUNCTION_SCRIPT);
    
    if (is_inner_class) {
        // Inner class: move to pre-defined local (defined before method compilation
        // so static methods access the class via upvalue, not shared)
        EMIT_ABC(ctx, compiler, OP_MOVE, inner_class_local->reg, class_reg, 0);
        reg_free(compiler, class_reg);
        class_reg = inner_class_local->reg;
    } else if (is_repl_top_level(ctx, compiler)) {
        // REPL: store in shared variable for cross-input persistence
        EMIT_ABX(ctx, compiler, OP_SETSHARED, class_reg, class_global_idx);
    } else if (is_module_level && !ctx->repl_mode) {
        // Module-level: local + shared (test runner and inner functions access via OP_GETSHARED)
        XrLocalInfo *local = scope_define_local(ctx, compiler, class_name_str);
        local->is_const = true;
        EMIT_ABC(ctx, compiler, OP_MOVE, local->reg, class_reg, 0);
        EMIT_ABX(ctx, compiler, OP_SETSHARED, class_reg, class_global_idx);
        reg_free(compiler, class_reg);
        class_reg = local->reg;
    } else {
        // Other non-module top-level: use shared variable
        EMIT_ABX(ctx, compiler, OP_SETSHARED, class_reg, class_global_idx);
    }
    
    // 8. Handle inheritance
    if (node->super_name != NULL) {
        int super_reg = reg_alloc(ctx, compiler);
        
        if (node->super_module != NULL) {
            // Module member access form: extends module.Class
            // First load module (priority: local variable, then upvalue, finally global)
            XrString *module_name = xr_compile_time_intern(ctx->X, node->super_module, strlen(node->super_module));
            
            // Find local variable
            XrLocalInfo *local = compiler_get_local_by_name(compiler, node->super_module);
            if (local) {
                EMIT_ABC(ctx, compiler, OP_MOVE, super_reg, local->reg, 0);
            } else {
                // Find upvalue
                int upvalue_idx = scope_resolve_upvalue(ctx, compiler, module_name);
                if (upvalue_idx >= 0) {
                    // Module names are always const, direct UPVAL_GET
                    emit_abc(compiler->emitter, OP_UPVAL_GET, super_reg, upvalue_idx, 0);
                } else {
                    // Fallback: check shared, then predefined globals
                    int si = shared_get_in_scope(ctx, compiler, module_name);
                    if (si >= 0) {
                        EMIT_ABX(ctx, compiler, OP_GETSHARED, super_reg, si);
                    } else {
                        int gi = builtin_get(ctx, module_name);
                        if (gi >= 0) {
                            EMIT_ABX(ctx, compiler, OP_GETBUILTIN, super_reg, gi);
                        }
                    }
                }
            }
            
            // Then get class member (OP_GETPROP uses symbol not constant index)
            int global_sym = xr_symbol_register_in_table((XrSymbolTable*)xr_isolate_get_symbol_table(ctx->X), node->super_name);
            int local_sym = xr_proto_add_symbol(compiler->proto, global_sym);
            EMIT_ABC(ctx, compiler, OP_GETPROP, super_reg, super_reg, local_sym);
        } else {
            /*
             * Simple form: extends Class
             * Parent may come from import (local/upvalue) or same module definition (global)
             */
            XrString *super_name = xr_compile_time_intern(ctx->X, node->super_name, strlen(node->super_name));
            XrLocalInfo *local = compiler_get_local_by_name(compiler, node->super_name);
            
            if (local) {
                // Parent is local variable (from import)
                EMIT_ABC(ctx, compiler, OP_MOVE, super_reg, local->reg, 0);
            } else {
                int upvalue_idx = scope_resolve_upvalue(ctx, compiler, super_name);
                if (upvalue_idx >= 0) {
                    // Parent class name is always const, direct UPVAL_GET
                    emit_abc(compiler->emitter, OP_UPVAL_GET, super_reg, upvalue_idx, 0);
                } else {
                    // Parent is shared variable (same module definition)
                    int si = shared_get_in_scope(ctx, compiler, super_name);
                    if (si >= 0) {
                        EMIT_ABX(ctx, compiler, OP_GETSHARED, super_reg, si);
                    } else {
                        int gi = builtin_get(ctx, super_name);
                        if (gi >= 0) {
                            EMIT_ABX(ctx, compiler, OP_GETBUILTIN, super_reg, gi);
                        }
                    }
                }
            }
        }
        
        // Set inheritance relationship
        EMIT_ABC(ctx, compiler, OP_INHERIT, class_reg, super_reg, 0);
        
        reg_free(compiler, super_reg);
    }
    
    // 9. Call static constructor (if any)
    if (desc->clinit_proto_index >= 0) {
        /*
         * Generate OP_CLINIT_CALL instruction
         * At this point class is already set as global, GETGLOBAL in static constructor works normally
         * Parameters: A=class register, B=descriptor constant index, C=unused
         */
        EMIT_ABC(ctx, compiler, OP_CLINIT_CALL, class_reg, desc_idx, 0);
    }
    
    // 10. Cleanup
    reg_free(compiler, class_reg);
}

/*
 * Compile class declaration
 */
void compile_class(XrCompilerContext *ctx, XrCompiler *compiler, ClassDeclNode *node) {
    XR_DCHECK(ctx != NULL, "compile_class: NULL ctx");
    XR_DCHECK(compiler != NULL, "compile_class: NULL compiler");
    XR_DCHECK(node != NULL, "compile_class: NULL node");
    // Use ClassDescriptor approach (supports inheritance, static members, interfaces and all OOP features)
    compile_class_with_descriptor(ctx, compiler, node);
}
