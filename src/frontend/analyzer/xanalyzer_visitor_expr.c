/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xanalyzer_visitor_expr.c - Pass 2 expression type inference visitors
 *
 * KEY CONCEPT:
 *   Type inference for all expression kinds: literals, variables,
 *   operators, calls, member access, match expressions, optional
 *   chains, closures, and generic substitution.
 */

#include "xanalyzer_visitor_internal.h"
#include "../../base/xchecks.h"

/* ============================================================================
 * Pass 2: Expression Visitors
 * ============================================================================
 * Type inference for all expression kinds: literals, variables, operators,
 * function calls, member access, containers, etc.
 * ========================================================================== */

// Forward declarations for static expression visitors
XrType *xa_visit_nullish_coalesce(XaInferContext *ctx, AstNode *node);
XrType *xa_visit_optional_chain(XaInferContext *ctx, AstNode *node);
XrType *xa_visit_match_expr(XaInferContext *ctx, AstNode *node);

// Check if an AST node is a typeof() call, return the argument variable name
const char *get_typeof_arg_name(AstNode *node) {
    if (!node || node->type != AST_CALL_EXPR) return NULL;
    CallExprNode *call = &node->as.call_expr;
    if (!call->callee || call->callee->type != AST_VARIABLE) return NULL;
    if (strcmp(call->callee->as.variable.name, "typeof") != 0) return NULL;
    if (call->arg_count != 1 || !call->arguments[0]) return NULL;
    if (call->arguments[0]->type != AST_VARIABLE) return NULL;
    return call->arguments[0]->as.variable.name;
}

XrType *xa_visit_variable(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node) return xr_type_new_unknown();
    
    const char *name = node->as.variable.name;
    XaSymbol *sym = xa_scope_lookup(ctx->analyzer->current_scope, name);
    
    // Also check global scope for classes (needed when called from compile phase)
    if (!sym && ctx->analyzer->global_scope) {
        sym = xa_scope_lookup(ctx->analyzer->global_scope, name);
    }
    
    if (!sym) {
        // Undeclared variable — detect common cross-language mistakes
        XrLocation loc = { .file = ctx->file_path, .line = node->line, .column = node->column };
        char msg[256];
        if (strcmp(name, "True") == 0) {
            snprintf(msg, sizeof(msg), "Undeclared variable 'True'. Use lowercase 'true' in Xray");
        } else if (strcmp(name, "False") == 0) {
            snprintf(msg, sizeof(msg), "Undeclared variable 'False'. Use lowercase 'false' in Xray");
        } else if (strcmp(name, "None") == 0) {
            snprintf(msg, sizeof(msg), "Undeclared variable 'None'. Use 'null' instead of 'None' in Xray");
        } else if (strcmp(name, "nil") == 0) {
            snprintf(msg, sizeof(msg), "Undeclared variable 'nil'. Use 'null' instead of 'nil' in Xray");
        } else if (strcmp(name, "undefined") == 0) {
            snprintf(msg, sizeof(msg), "Undeclared variable 'undefined'. Use 'null' in Xray");
        } else if (strcmp(name, "self") == 0) {
            snprintf(msg, sizeof(msg), "Undeclared variable 'self'. Use 'this' to refer to the current instance in Xray");
        } else {
            snprintf(msg, sizeof(msg), "Undeclared variable '%s'", name);
        }
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_UNDEFINED_VAR, msg, &loc);
        return xr_type_new_unknown();
    }
    
    // Record dependency: current function depends on this symbol
    if (ctx->current_function && ctx->analyzer->incremental) {
        XaIncrementalCtx *incr = (XaIncrementalCtx*)ctx->analyzer->incremental;
        xa_dep_add(incr, ctx->current_function->id, sym->id, XA_DEP_REFERENCE);
    }
    
    // Record reference location for Find References
    XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, sym);
    if (links) {
        uint32_t end_col = node->column + (name ? strlen(name) : 0);
        xa_symbol_add_ref(links, node->line, node->column, end_col, false);
    }
    
    // Definite assignment check: warn if variable used before initialization
    if (links && !links->is_definitely_assigned &&
        sym->kind == XA_SYM_VARIABLE && !sym->is_builtin) {
        XrLocation loc = { .file = ctx->file_path, .line = node->line, .column = node->column };
        char msg[256];
        snprintf(msg, sizeof(msg), "Variable '%s' is used before being assigned", name);
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
            XR_ERR_ANALYZE_TYPE_MISMATCH, msg, &loc);
    }
    
    // Get declared type
    XrType *declared_type = xa_analyzer_get_type(ctx->analyzer, sym);
    
    // Apply flow-based type narrowing (only for variables, not functions/classes)
    if (ctx->flow && ctx->flow->current_flow && declared_type &&
        sym->kind == XA_SYM_VARIABLE) {
        XrType *narrowed = xa_flow_get_type_of_reference(
            ctx->flow, name, declared_type, ctx->flow->current_flow, ctx->cache);
        // Never means unreachable flow path — fall back to declared type
        if (narrowed && narrowed != declared_type && !XR_TYPE_IS_NEVER(narrowed)) {
            node->compile_type = narrowed;
            return narrowed;
        }
    }
    
    // Store type on AST node for code generation phase
    node->compile_type = declared_type;
    return declared_type;
}

XrType *xa_visit_binary(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node) return xr_type_new_unknown();
    
    XrType *left = xa_visit_infer_expr(ctx, node->as.binary.left);
    XrType *right = xa_visit_infer_expr(ctx, node->as.binary.right);
    
    // Deterministic result types: language rules independent of operand types
    switch (node->type) {
        // Comparison → always bool
        case AST_BINARY_EQ: case AST_BINARY_NE:
        case AST_BINARY_EQ_STRICT: case AST_BINARY_NE_STRICT:
        case AST_BINARY_LT: case AST_BINARY_LE:
        case AST_BINARY_GT: case AST_BINARY_GE:
            return xr_type_new_bool();
        // Logical → always bool
        case AST_BINARY_AND: case AST_BINARY_OR:
            return xr_type_new_bool();
        // Bitwise → always int
        case AST_BINARY_BAND: case AST_BINARY_BOR: case AST_BINARY_BXOR:
        case AST_BINARY_LSHIFT: case AST_BINARY_RSHIFT:
            return xr_type_new_int();
        default:
            break;
    }
    
    // Arithmetic: result depends on operand types
    if (XR_TYPE_IS_UNKNOWN(left) || XR_TYPE_IS_UNKNOWN(right)) {
        return xr_type_new_unknown();
    }
    
    switch (node->type) {
        case AST_BINARY_ADD:
            // B+ rule: string + string => string (compiler rejects string + non-string)
            if (XR_TYPE_IS_STRING(left) && XR_TYPE_IS_STRING(right)) {
                return xr_type_new_string();
            }
            // string + unknown => string (dynamic concat) in OP_ADD)
            if (XR_TYPE_IS_STRING(left) || XR_TYPE_IS_STRING(right)) {
                return xr_type_new_string();
            }
            // fall through to numeric rules
        case AST_BINARY_SUB:
        case AST_BINARY_MUL:
        case AST_BINARY_DIV:
        case AST_BINARY_MOD:
            if (XR_TYPE_IS_FLOAT(left) || XR_TYPE_IS_FLOAT(right)) {
                return xr_type_new_float();
            }
            if (XR_TYPE_IS_INT(left) && XR_TYPE_IS_INT(right)) {
                return xr_type_new_int();
            }
            return xr_type_new_unknown();
            
        default:
            return xr_type_new_unknown();
    }
}

XrType *xa_visit_unary(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node) return xr_type_new_unknown();
    
    XrType *operand = xa_visit_infer_expr(ctx, node->as.unary.operand);
    
    switch (node->type) {
        case AST_UNARY_NEG:
            return operand;  // -x has same type as x
        case AST_UNARY_NOT:
            return xr_type_new_bool();
        case AST_UNARY_BNOT:
            return xr_type_new_int();
        default:
            return xr_type_new_unknown();
    }
}

/* ----------------------------------------------------------------------------
 * Function Call Type Inference
 * Handles: argument count/type checking, generic type argument validation,
 * type parameter inference for generic functions, and callback type inference
 * for container methods (map, filter, reduce, etc.)
 * -------------------------------------------------------------------------- */
XrType *xa_visit_call(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node) return xr_type_new_unknown();
    
    CallExprNode *call = &node->as.call_expr;
    
    // Record dependency: current function depends on called function
    XaSymbol *fn_sym = NULL;
    XaSymbolLinks *fn_links = NULL;
    
    if (call->callee && call->callee->type == AST_VARIABLE) {
        const char *fn_name = call->callee->as.variable.name;
        fn_sym = xa_scope_lookup(ctx->analyzer->current_scope, fn_name);
        if (fn_sym && fn_sym->kind == XA_SYM_FUNCTION) {
            fn_links = xa_analyzer_get_links(ctx->analyzer, fn_sym);
            
            if (ctx->current_function && ctx->analyzer->incremental) {
                XaIncrementalCtx *incr = (XaIncrementalCtx*)ctx->analyzer->incremental;
                xa_dep_add(incr, ctx->current_function->id, fn_sym->id, XA_DEP_CALL);
            }
        }
    }
    
    // Check generic type argument count and constraints
    if (call->type_arg_count > 0 && fn_links) {
        int expected_count = xa_symbol_links_get_type_param_count(fn_links);
        
        // Check count matches
        if (call->type_arg_count != expected_count) {
            XrLocation loc = { .file = ctx->file_path, .line = node->line, .column = node->column };
            char msg[256];
            const char *fn_name = call->callee && call->callee->type == AST_VARIABLE 
                ? call->callee->as.variable.name : "function";
            snprintf(msg, sizeof(msg), 
                "Generic function '%s' expects %d type argument(s), but got %d",
                fn_name, expected_count, call->type_arg_count);
            xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_GENERIC_COUNT, msg, &loc);
        }
        
        // Check constraints
        for (int i = 0; i < call->type_arg_count && i < expected_count; i++) {
            XrType *type_arg = call->type_args[i];
            XrType *constraint = xa_symbol_links_get_type_param_constraint(fn_links, i);
            
            if (constraint && type_arg && !xr_type_satisfies_constraint(type_arg, constraint)) {
                XrLocation loc = { .file = ctx->file_path, .line = node->line, .column = node->column };
                const char *param_name = xa_symbol_links_get_type_param_name(fn_links, i);
                char msg[256];
                snprintf(msg, sizeof(msg), 
                    "Type '%s' does not satisfy constraint '%s' for type parameter '%s'",
                    xr_type_to_string(type_arg), xr_type_to_string(constraint),
                    param_name ? param_name : "?");
                xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_GENERIC_CONSTRAINT, msg, &loc);
            }
        }
    }
    
    // Check for method call pattern: container.method(callback)
    // to enable generic type inference for callbacks
    XrType *container_elem_type = NULL;
    const char *method_name = NULL;
    XrType *callee_obj_type = NULL;  // cached once; reused by filter/reduce/generic substitution
    
    if (call->callee && call->callee->type == AST_MEMBER_ACCESS) {
        MemberAccessNode *ma = &call->callee->as.member_access;
        method_name = ma->name;
        callee_obj_type = xa_visit_infer_expr(ctx, ma->object);
        
        // Extract element type from container
        if (XR_TYPE_IS_ARRAY(callee_obj_type) && callee_obj_type->container.element_type) {
            container_elem_type = callee_obj_type->container.element_type;
        } else if ((callee_obj_type->kind == XR_KIND_SET) && callee_obj_type->container.element_type) {
            container_elem_type = callee_obj_type->container.element_type;
        } else if (XR_TYPE_IS_MAP(callee_obj_type)) {
            // For Map, callback gets (value, key) or (key, value) depending on method
            if (method_name && strcmp(method_name, "forEach") == 0) {
                container_elem_type = callee_obj_type->map.value_type;
            }
        }
    }
    
    XrType *callee_type = xa_visit_infer_expr(ctx, call->callee);
    
    // unknown type is callable (dynamic)
    if (XR_TYPE_IS_UNKNOWN(callee_type)) {
        // Check if callee is a class name - if so, return instance type
        if (call->callee && call->callee->type == AST_VARIABLE) {
            const char *name = call->callee->as.variable.name;
            XaSymbol *sym = xa_scope_lookup(ctx->analyzer->global_scope, name);
            if (sym && sym->kind == XA_SYM_CLASS) {
                XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, sym);
                if (links && links->class_info) {
                    return xr_type_new_instance(links->class_info);
                }
            }
        }
        return xr_type_new_unknown();
    }
    
    // Class constructor call: ClassName(args) returns instance type
    if (callee_type->kind == XR_KIND_CLASS) {
        // Look up class symbol to get XrClassInfo
        if (call->callee && call->callee->type == AST_VARIABLE) {
            const char *class_name = call->callee->as.variable.name;
            XaSymbol *class_sym = xa_scope_lookup(ctx->analyzer->global_scope, class_name);
            if (class_sym && class_sym->kind == XA_SYM_CLASS) {
                XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, class_sym);
                if (links && links->class_info) {
                    return xr_type_new_instance(links->class_info);
                }
            }
        }
        return xr_type_new_unknown();
    }
    
    // Check if callee is callable
    if (!XR_TYPE_IS_FUNCTION(callee_type)) {
        // Builtin method call: container.method() where member_access returned
        // the method's return type directly (e.g. arr.length() → int).
        // Accept primitive/container return types without warning.
        if (call->callee && call->callee->type == AST_MEMBER_ACCESS &&
            callee_type && !XR_TYPE_IS_UNKNOWN(callee_type)) {
            return callee_type;
        }
        XrLocation loc = { .file = ctx->file_path, .line = node->line, .column = node->column };
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_NOT_CALLABLE,
            "Value is not callable", &loc);
        return xr_type_new_unknown();
    }
    
    // Infer argument types
    int arg_count = call->arg_count;
    int param_count = callee_type->function.param_count;
    bool is_variadic = callee_type->function.is_variadic;
    
    // Check argument count (use min_params for functions with default parameters)
    int min_params = callee_type->function.min_params;
    if (arg_count < min_params && !is_variadic) {
        XrLocation loc = { .file = ctx->file_path, .line = node->line, .column = node->column };
        char msg[128];
        if (min_params == param_count) {
            snprintf(msg, sizeof(msg), "Expected %d argument(s), but got %d", param_count, arg_count);
        } else {
            snprintf(msg, sizeof(msg), "Expected %d to %d argument(s), but got %d", min_params, param_count, arg_count);
        }
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_WRONG_ARG_COUNT, msg, &loc);
    } else if (arg_count > param_count && !is_variadic) {
        XrLocation loc = { .file = ctx->file_path, .line = node->line, .column = node->column };
        char msg[128];
        snprintf(msg, sizeof(msg), "Expected %d argument(s), but got %d", param_count, arg_count);
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_WRONG_ARG_COUNT, msg, &loc);
    }
    
    // Check argument types with generic inference for callbacks
    XrType **param_types = callee_type->function.param_types;
    
    // Save and set callback context for generic inference
    XrType *saved_elem_type = ctx->callback_element_type;
    XrType *saved_index_type = ctx->callback_index_type;
    XrType *saved_acc_type = ctx->callback_accumulator_type;
    XrType *saved_arr_type = ctx->callback_array_type;
    
    // Set callback context if this is a container method with callbacks
    if (container_elem_type && method_name) {
        // Methods that take callbacks: map, filter, forEach, find, etc.
        if (strcmp(method_name, "map") == 0 || strcmp(method_name, "filter") == 0 ||
            strcmp(method_name, "forEach") == 0 || strcmp(method_name, "find") == 0 ||
            strcmp(method_name, "findIndex") == 0 || strcmp(method_name, "every") == 0 ||
            strcmp(method_name, "some") == 0) {
            ctx->callback_element_type = container_elem_type;
            ctx->callback_index_type = xr_type_new_int();
        }
        // reduce: fn(acc, item, index, array) => acc
        else if (strcmp(method_name, "reduce") == 0) {
            ctx->callback_element_type = container_elem_type;
            ctx->callback_index_type = xr_type_new_int();
            // Get accumulator type from second argument (initial value)
            if (arg_count >= 2 && call->arguments[1]) {
                ctx->callback_accumulator_type = xa_visit_infer_expr(ctx, call->arguments[1]);
            }
            // Store array type for 4th callback param
            ctx->callback_array_type = callee_obj_type;
        }
    }
    
    for (int i = 0; i < arg_count && i < param_count; i++) {
        XrType *param_type = param_types ? param_types[i] : NULL;
        
        // Bidirectional inference: propagate parameter type to argument
        XrType *saved_expected = ctx->expected_type;
        if (param_type && !XR_TYPE_IS_UNKNOWN(param_type)) {
            ctx->expected_type = param_type;
        }
        XrType *arg_type = xa_visit_infer_expr(ctx, call->arguments[i]);
        ctx->expected_type = saved_expected;
        
        if (param_type && !XR_TYPE_IS_UNKNOWN(param_type)) {
            XrLocation loc = { .file = ctx->file_path, .line = node->line, .column = node->column };
            {
                // Check null safety then assignability
                bool null_err = xa_check_null_safety(ctx->analyzer, param_type, arg_type,
                    "Argument", &loc);
                if (!null_err && !xr_type_assignable(param_type, arg_type)) {
                    char msg[256];
                    snprintf(msg, sizeof(msg),
                        "Argument %d: type '%s' is not assignable to parameter type '%s'",
                        i + 1, xr_type_to_string(arg_type), xr_type_to_string(param_type));
                    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_ARG_TYPE, msg, &loc);
                }
            }
        }
    }
    
    // Restore callback context
    ctx->callback_element_type = saved_elem_type;
    ctx->callback_index_type = saved_index_type;
    ctx->callback_accumulator_type = saved_acc_type;
    ctx->callback_array_type = saved_arr_type;
    
    // Call-site parameter type propagation: for unannotated parameters,
    // record the inferred argument type so codegen can use it for optimization.
    if (fn_links && arg_count > 0) {
        // Ensure inferred_param_types array is allocated
        if (!fn_links->inferred_param_types && fn_links->param_count > 0) {
            fn_links->inferred_param_types = xr_calloc(fn_links->param_count, sizeof(XrType*));
            fn_links->inferred_param_count = fn_links->param_count;
        }
        for (int i = 0; i < arg_count && i < fn_links->inferred_param_count; i++) {
            // Only propagate for unannotated params (declared_type is NULL or unknown)
            XrType *declared = (fn_links->param_types && i < fn_links->param_count)
                               ? fn_links->param_types[i] : NULL;
            if (declared && !XR_TYPE_IS_UNKNOWN(declared)) continue;  // explicitly typed
            
            XrType *arg_type = call->arguments[i]->compile_type;
            if (!arg_type || XR_TYPE_IS_UNKNOWN(arg_type))
                continue;
            
            XrType **slot = &fn_links->inferred_param_types[i];
            if (!*slot) {
                *slot = arg_type;  // First observation
            } else if (!xr_type_equals(*slot, arg_type)) {
                // If types differ only in nullability, widen to nullable
                XrType *a = xr_type_non_nullable(*slot);
                XrType *b = xr_type_non_nullable(arg_type);
                if (a && b && xr_type_equals(a, b)) {
                    *slot = xr_type_make_nullable(a);
                } else {
                    *slot = xr_type_new_unknown();  // Incompatible callers
                }
            }
        }
    }
    
    // Infer remaining arguments (for type checking side effects)
    for (int i = param_count; i < arg_count; i++) {
        xa_visit_infer_expr(ctx, call->arguments[i]);
    }
    
    XrType *return_type = callee_type->function.return_type;
    
    // G2: Override return type for container methods using callback return type
    if (container_elem_type && method_name && arg_count >= 1) {
        if (strcmp(method_name, "map") == 0) {
            // arr.map(fn) -> Array<callback_return_type>
            // Use cached type from argument evaluation above (avoid re-evaluation
            // which would lose callback context)
            XrType *cb_type = call->arguments[0]->compile_type;
            if (XR_TYPE_IS_FUNCTION(cb_type) && cb_type->function.return_type &&
                !XR_TYPE_IS_UNKNOWN(cb_type->function.return_type)) {
                return_type = xr_type_new_array(cb_type->function.return_type);
            }
        } else if (strcmp(method_name, "filter") == 0) {
            // arr.filter(fn) -> same Array type as source
            if (XR_TYPE_IS_ARRAY(callee_obj_type)) {
                return_type = callee_obj_type;
            }
        } else if (strcmp(method_name, "reduce") == 0 && arg_count >= 2) {
            // arr.reduce(fn, init) -> type of init value
            XrType *init_type = xa_visit_infer_expr(ctx, call->arguments[1]);
            if (init_type && !XR_TYPE_IS_UNKNOWN(init_type)) {
                return_type = init_type;
            }
        } else if (strcmp(method_name, "find") == 0) {
            // arr.find(fn) -> element_type? (nullable)
            return_type = xr_type_make_nullable(xr_type_copy(container_elem_type));
        } else if (strcmp(method_name, "findIndex") == 0) {
            return_type = xr_type_new_int();
        } else if (strcmp(method_name, "every") == 0 || strcmp(method_name, "some") == 0) {
            return_type = xr_type_new_bool();
        }
    }
    
    // Apply type substitution for generic function calls
    if (return_type && fn_links) {
        return_type = xa_substitute_generic_call(ctx, fn_links, callee_type, return_type, call, arg_count);
    }
    
    // Apply type substitution for generic method calls: obj.method<T>()
    if (callee_obj_type && call->callee->type == AST_MEMBER_ACCESS) {
        MemberAccessNode *ma = &call->callee->as.member_access;
        
        // Look up method in class
        if (XR_TYPE_IS_INSTANCE(callee_obj_type) && callee_obj_type->instance.class_name) {
            XaSymbol *class_sym = xa_scope_lookup(ctx->analyzer->global_scope,
                                                  callee_obj_type->instance.class_name);
            if (class_sym && class_sym->kind == XA_SYM_CLASS) {
                XaSymbolLinks *class_links = xa_analyzer_get_links(ctx->analyzer, class_sym);
                if (class_links && class_links->class_info) {
                    XaSymbol *method_sym = xa_class_info_lookup_member(
                        class_links->class_info, ma->name);
                    if (method_sym && method_sym->kind == XA_SYM_METHOD) {
                        XaSymbolLinks *method_links = xa_analyzer_get_links(ctx->analyzer, method_sym);
                        if (method_links) {
                            // Apply method's own type parameters
                            return_type = xa_substitute_generic_call(ctx, method_links, callee_type,
                                                                      return_type, call, arg_count);
                            
                            // Also apply class type parameters substitution
                            int class_type_param_count = xa_symbol_links_get_type_param_count(class_links);
                            if (class_type_param_count > 0 && callee_obj_type->instance.type_arg_count > 0) {
                                const char **class_param_names = xr_malloc(sizeof(const char*) * class_type_param_count);
                                for (int i = 0; i < class_type_param_count; i++) {
                                    class_param_names[i] = xa_symbol_links_get_type_param_name(class_links, i);
                                }
                                return_type = xr_type_substitute(return_type, class_param_names,
                                    callee_obj_type->instance.type_args, callee_obj_type->instance.type_arg_count);
                                xr_free(class_param_names);
                            }
                        }
                    }
                }
            }
        }
    }
    
    return return_type ? return_type : xr_type_new_unknown();
}

/* ----------------------------------------------------------------------------
 * Member Access Type Inference
 * -------------------------------------------------------------------------- */
XrType *xa_visit_member_access(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node) return xr_type_new_unknown();
    
    MemberAccessNode *ma = &node->as.member_access;
    XrType *obj_type = xa_visit_infer_expr(ctx, ma->object);
    
    // Check module member access before the unknown-type early return (e.g., net.dial)
    if (XR_TYPE_IS_UNKNOWN(obj_type) && ma->object->type == AST_VARIABLE) {
        const char *var_name = ma->object->as.variable.name;
        XaSymbol *sym = xa_scope_lookup(ctx->analyzer->current_scope, var_name);
        if (sym && sym->kind == XA_SYM_MODULE) {
            XaSymbolLinks *sym_links = xa_analyzer_get_links(ctx->analyzer, sym);
            const char *mod_name = (sym_links && sym_links->module_name) 
                                   ? sym_links->module_name : var_name;
            const XaBuiltinModule *mod = xa_builtin_get_module_info(mod_name);
            if (mod) {
                // Look up function signature in module type table
                const char *sig = xa_builtin_get_module_func_signature(mod_name, ma->name);
                if (sig) {
                    // Parse complete function signature (params + return type)
                    return xa_builtin_parse_full_signature(sig);
                }
                // Known module but unknown member - still unknown for extensibility
                return xr_type_new_unknown();
            }
        }
    }
    
    // Class static member access: ClassName.staticMethod
    if (obj_type->kind == XR_KIND_CLASS && obj_type->instance.class_name) {
        XaSymbol *class_sym = xa_scope_lookup(ctx->analyzer->global_scope,
                                              obj_type->instance.class_name);
        if (class_sym && class_sym->kind == XA_SYM_CLASS) {
            XaSymbolLinks *class_links = xa_analyzer_get_links(ctx->analyzer, class_sym);
            if (class_links && class_links->class_info) {
                XaSymbol *member = xa_class_info_lookup_member(
                    class_links->class_info, ma->name);
                if (member) {
                    XaSymbolLinks *ml = xa_analyzer_get_links(ctx->analyzer, member);
                    if (ml && ml->type) return ml->type;
                }
            }
        }
    }

    // unknown type allows dynamic member access
    if (XR_TYPE_IS_UNKNOWN(obj_type)) {
        return xr_type_new_unknown();
    }
    
    // Handle built-in properties
    SymbolId prop_sym = xr_builtin_symbol_from_name(ma->name);
    if (prop_sym == SYMBOL_LENGTH) {
        if (XR_TYPE_IS_ARRAY(obj_type) || XR_TYPE_IS_STRING(obj_type) ||
            XR_TYPE_IS_MAP(obj_type) || (obj_type->kind == XR_KIND_SET)) {
            return xr_type_new_int();
        }
    }
    if (obj_type->kind == XR_KIND_CHANNEL) {
        if (prop_sym == SYMBOL_CANCELLED) return xr_type_new_bool();
    }
    
    // Handle built-in methods (return function type for method access)
    if (xa_builtin_is_method(obj_type, ma->name)) {
        const char *sig = xa_builtin_get_member_signature(obj_type, ma->name);
        if (sig) {
            XrType *fn_type = xa_builtin_parse_full_signature(sig);
            // Substitute generic type parameters with actual container types:
            //   Array<T>/Set<T>/Channel<T>: T -> element_type
            //   Map<K,V>: K -> key_type, V -> value_type
            if (fn_type) {
                if ((XR_TYPE_IS_ARRAY(obj_type) || obj_type->kind == XR_KIND_SET ||
                     obj_type->kind == XR_KIND_CHANNEL) &&
                    obj_type->container.element_type &&
                    !XR_TYPE_IS_UNKNOWN(obj_type->container.element_type)) {
                    const char *names[] = { "T" };
                    XrType *types[] = { obj_type->container.element_type };
                    fn_type = xr_type_substitute(fn_type, names, types, 1);
                } else if (XR_TYPE_IS_MAP(obj_type)) {
                    XrType *kt = obj_type->map.key_type;
                    XrType *vt = obj_type->map.value_type;
                    if (kt && !XR_TYPE_IS_UNKNOWN(kt) && vt && !XR_TYPE_IS_UNKNOWN(vt)) {
                        const char *names[] = { "K", "V" };
                        XrType *types[] = { kt, vt };
                        fn_type = xr_type_substitute(fn_type, names, types, 2);
                    }
                }
            }
            return fn_type;
        }
        // Fallback: return function with unknown return type
        XrType *return_type = xa_builtin_get_method_return_type(obj_type, ma->name);
        if (return_type) {
            return xr_type_new_function(NULL, 0, return_type, false);
        }
    }
    
    // Handle class instance members
    if (XR_TYPE_IS_INSTANCE(obj_type) && obj_type->instance.class_name) {
        XaSymbol *class_sym = xa_scope_lookup(ctx->analyzer->global_scope, 
            obj_type->instance.class_name);
        if (class_sym) {
            XaSymbolLinks *class_links = xa_analyzer_get_links(ctx->analyzer, class_sym);
            if (class_links && class_links->class_info) {
                XaSymbol *member = xa_class_info_lookup_member(class_links->class_info, ma->name);
                if (member) {
                    XaSymbolLinks *member_links = xa_analyzer_get_links(ctx->analyzer, member);
                    if (member_links && member_links->type) {
                        XrType *member_type = member_links->type;
                        
                        // Apply type substitution for generic instances
                        int type_param_count = xa_symbol_links_get_type_param_count(class_links);
                        if (type_param_count > 0 && obj_type->instance.type_arg_count > 0) {
                            const char **param_names = xr_malloc(sizeof(const char*) * type_param_count);
                            for (int i = 0; i < type_param_count; i++) {
                                param_names[i] = xa_symbol_links_get_type_param_name(class_links, i);
                            }
                            member_type = xr_type_substitute(member_type, param_names,
                                                             obj_type->instance.type_args,
                                                             obj_type->instance.type_arg_count);
                            xr_free(param_names);
                        }
                        return member_type;
                    }
                }
            }
        }
    }
    
    // Handle Json object field access.
    // Json is a dynamic container: field values can be any type at runtime
    // including null, so the result type is always nullable.
    if (XR_TYPE_IS_JSON(obj_type) && obj_type->object.field_count > 0) {
        if (obj_type->object.field_names && obj_type->object.field_types) {
            for (int i = 0; i < obj_type->object.field_count; i++) {
                if (obj_type->object.field_names[i] && 
                    strcmp(obj_type->object.field_names[i], ma->name) == 0) {
                    XrType *ft = obj_type->object.field_types[i];
                    if (!ft) return xr_type_new_unknown();
                    return xr_type_make_nullable(ft);
                }
            }
        }
        // Json allows extension, return unknown for unknown fields
        if (obj_type->object.allow_extension) {
            return xr_type_new_unknown();
        }
    }
    
    return xr_type_new_unknown();
}

XrType *xa_visit_index_get(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node) return xr_type_new_unknown();
    
    IndexGetNode *ig = &node->as.index_get;
    XrType *container = xa_visit_infer_expr(ctx, ig->array);
    
    if (XR_TYPE_IS_ARRAY(container) && container->container.element_type) {
        return container->container.element_type;
    }
    if (XR_TYPE_IS_MAP(container) && container->map.value_type) {
        return container->map.value_type;
    }
    if (XR_TYPE_IS_STRING(container)) {
        return xr_type_new_string();  // string[i] => string
    }
    
    // Json subscript access: json["key"] → Json (or schema field type if known)
    if (XR_TYPE_IS_JSON(container)) {
        // If index is a string literal and Json has schema, look up field type
        if (ig->index && ig->index->type == AST_LITERAL_STRING &&
                   container->object.field_count > 0 &&
                   container->object.field_names && container->object.field_types) {
                   const char *key = ig->index->as.literal.raw_value.string_val;
            for (int i = 0; i < container->object.field_count; i++) {
                if (container->object.field_names[i] &&
                    strcmp(container->object.field_names[i], key) == 0) {
                    XrType *ft = container->object.field_types[i];
                    if (ft) return xr_type_make_nullable(ft);
                }
            }
        }
        // No schema or unknown key → result is JsonValue (null|bool|int|float|string|Json)
        return xr_type_new_json_value();
    }
    
    return xr_type_new_unknown();
}

XrType *xa_visit_array_literal(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node) return xr_type_new_array(xr_type_new_unknown());
    
    ArrayLiteralNode *arr = &node->as.array_literal;
    if (arr->count == 0) {
        // Empty array: use expected type if available
        if (ctx->expected_type && XR_TYPE_IS_ARRAY(ctx->expected_type) &&
            ctx->expected_type->container.element_type) {
            return xr_type_new_array(ctx->expected_type->container.element_type);
        }
        return xr_type_new_array(xr_type_new_unknown());
    }
    
    // Propagate expected element type to children
    XrType *saved_expected = ctx->expected_type;
    if (ctx->expected_type && XR_TYPE_IS_ARRAY(ctx->expected_type) &&
        ctx->expected_type->container.element_type) {
        ctx->expected_type = ctx->expected_type->container.element_type;
    } else {
        ctx->expected_type = NULL;
    }
    
    // Infer element type from first element
    XrType *elem_type = xa_visit_infer_expr(ctx, arr->elements[0]);
    
    // Union with other element types
    for (int i = 1; i < arr->count; i++) {
        XrType *t = xa_visit_infer_expr(ctx, arr->elements[i]);
        if (!xr_type_equals(elem_type, t)) {
            elem_type = xr_type_union(elem_type, t);
        }
    }
    
    ctx->expected_type = saved_expected;
    return xr_type_new_array(elem_type);
}

XrType *xa_visit_map_literal(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node) return xr_type_new_map(xr_type_new_unknown(), xr_type_new_unknown());
    
    MapLiteralNode *map = &node->as.map_literal;
    if (map->count == 0) {
        // Empty map: use expected type if available
        if (ctx->expected_type && XR_TYPE_IS_MAP(ctx->expected_type)) {
            XrType *ek = ctx->expected_type->map.key_type;
            XrType *ev = ctx->expected_type->map.value_type;
            return xr_type_new_map(ek ? ek : xr_type_new_unknown(), ev ? ev : xr_type_new_unknown());
        }
        return xr_type_new_map(xr_type_new_unknown(), xr_type_new_unknown());
    }
    
    // Propagate expected value type to children
    XrType *saved_expected = ctx->expected_type;
    if (ctx->expected_type && XR_TYPE_IS_MAP(ctx->expected_type) &&
        ctx->expected_type->map.value_type) {
        ctx->expected_type = ctx->expected_type->map.value_type;
    } else {
        ctx->expected_type = NULL;
    }
    
    // Infer key/value types from first element
    XrType *key_type = xa_visit_infer_expr(ctx, map->keys[0]);
    XrType *val_type = xa_visit_infer_expr(ctx, map->values[0]);
    
    // Union with remaining elements (same pattern as array_literal)
    for (int i = 1; i < map->count; i++) {
        XrType *k = xa_visit_infer_expr(ctx, map->keys[i]);
        XrType *v = xa_visit_infer_expr(ctx, map->values[i]);
        if (!xr_type_equals(key_type, k)) {
            key_type = xr_type_union(key_type, k);
        }
        if (!xr_type_equals(val_type, v)) {
            val_type = xr_type_union(val_type, v);
        }
    }
    
    ctx->expected_type = saved_expected;
    return xr_type_new_map(key_type, val_type);
}

XrType *xa_visit_object_literal(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node) return xr_type_new_json();
    
    ObjectLiteralNode *obj = &node->as.object_literal;
    if (obj->count == 0) return xr_type_new_json();
    
    // Collect field names and types
    const char **field_names = (const char**)xr_malloc(sizeof(char*) * obj->count);
    XrType **field_types = (XrType**)xr_malloc(sizeof(XrType*) * obj->count);
    
    for (int i = 0; i < obj->count; i++) {
        // Get field name (computed properties have runtime-only keys)
        bool is_computed = obj->computed && obj->computed[i];
        if (is_computed) {
            field_names[i] = NULL;
        } else if (obj->keys[i] && obj->keys[i]->type == AST_VARIABLE) {
            field_names[i] = obj->keys[i]->as.variable.name;
        } else if (obj->keys[i] && obj->keys[i]->type == AST_LITERAL_STRING) {
            field_names[i] = obj->keys[i]->as.literal.raw_value.string_val;
        } else {
            field_names[i] = NULL;
        }
        
        // Infer field type
        field_types[i] = xa_visit_infer_expr(ctx, obj->values[i]);
        
        // Reject non-JSON-standard types in Json object literals
        if (field_types[i] && !xr_type_is_json_field_compatible(field_types[i])) {
            XrLocation loc = { .file = ctx->file_path,
                               .line = obj->values[i]->line,
                               .column = obj->values[i]->column };
            char msg[256];
            const char *fname = field_names[i] ? field_names[i] : "<computed>";
            snprintf(msg, sizeof(msg),
                "Json field '%s' has type '%s', which is not a valid JSON value type. "
                "Valid types: null, bool, int, float, string, Json, Array",
                fname, xr_type_to_string(field_types[i]));
            xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                XR_ERR_ANALYZE_TYPE_MISMATCH, msg, &loc);
        }
    }
    
    XrType *type = xr_type_new_json_with_fields(field_names, field_types, obj->count);
    type->object.allow_extension = true;  // Json objects allow extension by default
    
    xr_free(field_names);
    xr_free(field_types);
    
    return type;
}

XrType *xa_visit_new_expr(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node) return xr_type_new_unknown();
    
    NewExprNode *ne = &node->as.new_expr;
    
    // Look up class symbol to get XrClassInfo
    // Try current scope first, then global scope
    XaSymbol *class_sym = xa_scope_lookup(ctx->analyzer->current_scope, ne->class_name);
    if (!class_sym) {
        class_sym = xa_scope_lookup(ctx->analyzer->global_scope, ne->class_name);
    }
    
    XrClassInfo *class_info = NULL;
    XaSymbolLinks *class_links = NULL;
    if (class_sym && class_sym->kind == XA_SYM_CLASS) {
        class_links = xa_analyzer_get_links(ctx->analyzer, class_sym);
        if (class_links && class_links->class_info) {
            class_info = class_links->class_info;
        }
    }
    
    // Check type argument count matches type parameter count
    if (ne->type_arg_count > 0) {
        int expected = class_links ? xa_symbol_links_get_type_param_count(class_links) : 0;
        if (expected > 0 && ne->type_arg_count != expected) {
            XrLocation loc = { .file = ctx->file_path, .line = node->line, .column = node->column };
            char msg[256];
            snprintf(msg, sizeof(msg), 
                "Generic class '%s' expects %d type argument(s), but got %d",
                ne->class_name, expected, ne->type_arg_count);
            xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_GENERIC_COUNT, msg, &loc);
        }
    }
    
    // If we have generic type arguments, create a generic instance type
    if (ne->type_arg_count > 0) {
        // Check constructor argument types against substituted parameter types
        if (class_info && class_links && ne->arg_count > 0) {
            int type_param_count = xa_symbol_links_get_type_param_count(class_links);
            if (type_param_count > 0 && type_param_count == ne->type_arg_count) {
                XaSymbol *ctor = xa_class_info_lookup_member(class_info, XR_KEYWORD_CONSTRUCTOR);
                if (ctor && ctor->kind == XA_SYM_METHOD) {
                    XaSymbolLinks *ctor_links = xa_analyzer_get_links(ctx->analyzer, ctor);
                    if (ctor_links && ctor_links->type && XR_TYPE_IS_FUNCTION(ctor_links->type)) {
                        // Build param_names array
                        const char *param_names_buf[8];
                        const char **param_names = (type_param_count <= 8) 
                            ? param_names_buf : xr_malloc(sizeof(const char*) * type_param_count);
                        for (int i = 0; i < type_param_count; i++) {
                            param_names[i] = xa_symbol_links_get_type_param_name(class_links, i);
                        }
                        
                        int ctor_pc = ctor_links->type->function.param_count;
                        XrType **ctor_params = ctor_links->type->function.param_types;
                        int check_count = ctor_pc < ne->arg_count ? ctor_pc : ne->arg_count;
                        
                        for (int i = 0; i < check_count; i++) {
                            XrType *expected = ctor_params ? ctor_params[i] : NULL;
                            if (!expected || XR_TYPE_IS_UNKNOWN(expected)) continue;
                            // Substitute T -> actual type arg
                            XrType *resolved = xr_type_substitute(expected, param_names,
                                                                   ne->type_args, ne->type_arg_count);
                            if (resolved && !XR_TYPE_IS_UNKNOWN(resolved)) {
                                XrType *arg_type = xa_visit_infer_expr(ctx, ne->arguments[i]);
                                if (arg_type && !xr_type_assignable(resolved, arg_type)) {
                                    XrLocation loc = { .file = ctx->file_path, .line = node->line, .column = node->column };
                                    char msg[256];
                                    snprintf(msg, sizeof(msg),
                                        "Type '%s' is not assignable to parameter type '%s' in new %s<>()",
                                        xr_type_to_string(arg_type), xr_type_to_string(resolved), ne->class_name);
                                    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                        XR_ERR_ANALYZE_TYPE_MISMATCH, msg, &loc);
                                }
                            }
                        }
                        if (param_names != param_names_buf) xr_free((void*)param_names);
                    }
                }
            }
        }
        return xr_type_new_generic_instance(ne->class_name, class_info,
                                            ne->type_args, ne->type_arg_count);
    }
    
    // Infer type arguments from constructor parameters: new Box(42) -> Box<int>
    if (class_links && ne->arg_count > 0) {
        int type_param_count = xa_symbol_links_get_type_param_count(class_links);
        if (type_param_count > 0 && class_info) {
            // Look up constructor
            XaSymbol *ctor = xa_class_info_lookup_member(class_info, XR_KEYWORD_CONSTRUCTOR);
            if (ctor && ctor->kind == XA_SYM_METHOD) {
                XaSymbolLinks *ctor_links = xa_analyzer_get_links(ctx->analyzer, ctor);
                if (ctor_links && ctor_links->type && XR_TYPE_IS_FUNCTION(ctor_links->type)) {
                    XrType **ctor_params = ctor_links->type->function.param_types;
                    int ctor_param_count = ctor_links->type->function.param_count;
                    
                    // Build type parameter names
                    const char **param_names = xr_malloc(sizeof(const char*) * type_param_count);
                    for (int i = 0; i < type_param_count; i++) {
                        param_names[i] = xa_symbol_links_get_type_param_name(class_links, i);
                    }
                    
                    // Infer type arguments from constructor arguments
                    XrType **inferred_args = xr_malloc(sizeof(XrType*) * type_param_count);
                    bool all_inferred = true;
                    
                    for (int i = 0; i < type_param_count; i++) {
                        inferred_args[i] = NULL;
                        const char *tp_name = param_names[i];
                        
                        // Find constructor parameter that uses this type parameter
                        for (int j = 0; j < ctor_param_count && j < ne->arg_count; j++) {
                            XrType *pt = ctor_params ? ctor_params[j] : NULL;
                            if (pt && (pt->kind == XR_KIND_TYPE_PARAM) &&
                                pt->type_param.name && strcmp(pt->type_param.name, tp_name) == 0) {
                                // Infer from argument type
                                inferred_args[i] = xa_visit_infer_expr(ctx, ne->arguments[j]);
                                break;
                            }
                        }
                        
                        if (!inferred_args[i]) {
                            all_inferred = false;
                        }
                    }
                    
                    // If all type parameters were inferred, create generic instance
                    if (all_inferred) {
                        XrType *result = xr_type_new_generic_instance(ne->class_name, class_info,
                                                                       inferred_args, type_param_count);
                        xr_free(param_names);
                        // Don't free inferred_args - it's now owned by the type
                        return result;
                    }
                    
                    xr_free(param_names);
                    xr_free(inferred_args);
                }
            }
        }
    }
    
    // No type arguments - use regular instance or class type
    if (class_info) {
        XrType *inst_type = xr_type_new_instance(class_info);
        // Propagate is_value_type from class declaration (struct)
        if (class_links && class_links->type && class_links->type->is_value_type) {
            inst_type->is_value_type = true;
        }
        return inst_type;
    }
    
    // Fallback: create class type with class name
    if (ne->class_name) {
        XrType *cls_type = xr_type_new_class(ne->class_name);
        // Propagate is_value_type from class declaration (struct)
        if (class_links && class_links->type && class_links->type->is_value_type) {
            cls_type->is_value_type = true;
        }
        return cls_type;
    }
    return xr_type_new_unknown();
}

XrType *xa_visit_struct_literal(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node) return xr_type_new_unknown();
    
    StructLiteralNode *sl = &node->as.struct_literal;
    const char *struct_name = sl->struct_name;
    
    // Infer field value types (for side effects / type checking)
    for (int i = 0; i < sl->field_count; i++) {
        xa_visit_infer_expr(ctx, sl->field_values[i]);
    }
    
    // Look up struct symbol
    XaSymbol *class_sym = xa_scope_lookup(ctx->analyzer->current_scope, struct_name);
    if (!class_sym) {
        class_sym = xa_scope_lookup(ctx->analyzer->global_scope, struct_name);
    }
    
    if (class_sym && class_sym->kind == XA_SYM_CLASS) {
        XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, class_sym);
        if (links && links->class_info) {
            XrType *inst_type = xr_type_new_instance(links->class_info);
            if (links->type && links->type->is_value_type) {
                inst_type->is_value_type = true;
            }
            return inst_type;
        }
        if (links && links->type) {
            return links->type;
        }
    }
    
    if (struct_name) {
        XrType *t = xr_type_new_class(struct_name);
        t->is_value_type = true;
        return t;
    }
    return xr_type_new_unknown();
}

XrType *xa_visit_ternary(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node) return xr_type_new_unknown();
    
    TernaryNode *tern = &node->as.ternary;
    // Bidirectional inference: propagate outer expected_type to both branches
    // (expected_type is already set by the caller, just pass through)
    XrType *then_type = xa_visit_infer_expr(ctx, tern->true_expr);
    XrType *else_type = xa_visit_infer_expr(ctx, tern->false_expr);
    
    if (xr_type_equals(then_type, else_type)) {
        return then_type;
    }
    return xr_type_union(then_type, else_type);
}

/* ----------------------------------------------------------------------------
 * Nullish Coalesce: a ?? b
 * If a is T?, result is T | typeof(b). If a is T (non-nullable), result is T.
 * -------------------------------------------------------------------------- */
XrType *xa_visit_nullish_coalesce(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node) return xr_type_new_unknown();
    
    XrType *left = xa_visit_infer_expr(ctx, node->as.binary.left);
    XrType *right = xa_visit_infer_expr(ctx, node->as.binary.right);
    
    // If left is nullable (T?), strip null and union with right
    // T? ?? U => T | U (most common: T? ?? T => T)
    XrType *non_null_left = xr_type_non_nullable(left);
    if (non_null_left != left) {
        // left was nullable, result is non-null version unified with right
        if (xr_type_equals(non_null_left, right)) {
            return non_null_left;
        }
        return xr_type_union(non_null_left, right);
    }
    
    // Left is not nullable, ?? is a no-op, return left type
    return left;
}

/* ----------------------------------------------------------------------------
 * Optional Chain: obj?.prop, obj?.[index], obj?.method()
 * Result is always nullable: typeof(obj.prop) | null => T?
 * -------------------------------------------------------------------------- */
XrType *xa_visit_optional_chain(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node) return xr_type_new_unknown();
    
    XrType *obj_type = xa_visit_infer_expr(ctx, node->as.optional_chain.object);
    
    // If object is unknown, result is unknown
    if (XR_TYPE_IS_UNKNOWN(obj_type)) {
        return xr_type_new_unknown();
    }
    
    // Strip nullable from object for member lookup
    XrType *base_type = xr_type_non_nullable(obj_type);
    
    // Property access: obj?.name
    if (node->as.optional_chain.name) {
        // Reuse member access logic by creating a temporary lookup
        // For now, handle common cases inline
        const char *prop_name = node->as.optional_chain.name;
        
        // Built-in properties — result is nullable (object may be null)
        if ((XR_TYPE_IS_ARRAY(base_type) || XR_TYPE_IS_STRING(base_type)) &&
            xr_builtin_symbol_from_name(prop_name) == SYMBOL_LENGTH) {
            return xr_type_make_nullable(xr_type_new_int());
        }
        
        // Class instance member
        if (XR_TYPE_IS_INSTANCE(base_type) && base_type->instance.class_name) {
            XaSymbol *class_sym = xa_scope_lookup(ctx->analyzer->global_scope,
                                                   base_type->instance.class_name);
            if (class_sym && class_sym->kind == XA_SYM_CLASS) {
                XaSymbolLinks *class_links = xa_analyzer_get_links(ctx->analyzer, class_sym);
                if (class_links && class_links->class_info) {
                    XaSymbol *member = xa_class_info_lookup_member(class_links->class_info, prop_name);
                    if (member) {
                        XaSymbolLinks *ml = xa_analyzer_get_links(ctx->analyzer, member);
                        if (ml && ml->type) {
                            XrType *result = xr_type_copy(ml->type);
                            if (result) result->is_nullable = true;
                            return result;
                        }
                    }
                }
            }
        }
        
        // Json field access
        if (XR_TYPE_IS_JSON(base_type) && base_type->object.field_count > 0) {
            for (int i = 0; i < base_type->object.field_count; i++) {
                if (base_type->object.field_names[i] &&
                    strcmp(base_type->object.field_names[i], prop_name) == 0) {
                    XrType *result = xr_type_copy(base_type->object.field_types[i]);
                    if (result) result->is_nullable = true;
                    return result;
                }
            }
        }
    }
    
    // Index access: obj?.[index]
    if (node->as.optional_chain.index) {
        xa_visit_infer_expr(ctx, node->as.optional_chain.index);
        
        if (XR_TYPE_IS_ARRAY(base_type) && base_type->container.element_type) {
            XrType *result = xr_type_copy(base_type->container.element_type);
            if (result) result->is_nullable = true;
            return result;
        }
        if (XR_TYPE_IS_MAP(base_type) && base_type->map.value_type) {
            XrType *result = xr_type_copy(base_type->map.value_type);
            if (result) result->is_nullable = true;
            return result;
        }
    }
    
    // Fallback: any?
    return xr_type_new_unknown();
}

/* as type cast: expr as T — returns T (non-safe), or T? (safe)
 * Analyzer simply returns the target type without checking operand type.
 * Runtime will perform the actual type check. */
XrType *xa_visit_as_expr(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node) return xr_type_new_unknown();
    // Visit operand to ensure it's analyzed (side effects, narrowing)
    xa_visit_infer_expr(ctx, node->as.as_expr.expr);
    XrType *target = node->as.as_expr.type;
    if (!target) return xr_type_new_unknown();
    return target;
}

/* Force unwrap: expr! strips nullable from T? to produce T.
 * If operand is already non-nullable, the ! is a no-op (no warning for now).
 * If operand is null type, the ! is always a runtime panic. */
XrType *xa_visit_force_unwrap(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node) return xr_type_new_unknown();
    XrType *inner = xa_visit_infer_expr(ctx, node->as.unary.operand);
    if (!inner) return xr_type_new_unknown();
    // Strip nullable: T? -> T
    if (inner->is_nullable) {
        return xr_type_non_nullable(inner);
    }
    // Already non-nullable or any: return as-is
    return inner;
}

// Forward declaration (defined below in narrowing section)
// Defined in xanalyzer_visitor_stmt.c

// Helper: map Type.xxx member name to XrTypeKind for typeof exhaustiveness
static XrTypeKind type_member_to_kind(const char *name) {
    if (!name) return XR_KIND_COUNT;
    if (strcmp(name, "int") == 0)      return XR_KIND_INT;
    if (strcmp(name, "float") == 0)    return XR_KIND_FLOAT;
    if (strcmp(name, "string") == 0)   return XR_KIND_STRING;
    if (strcmp(name, "bool") == 0)     return XR_KIND_BOOL;
    if (strcmp(name, "null") == 0)     return XR_KIND_NULL;
    if (strcmp(name, "Array") == 0)    return XR_KIND_ARRAY;
    if (strcmp(name, "Map") == 0)      return XR_KIND_MAP;
    if (strcmp(name, "Set") == 0)      return XR_KIND_SET;
    if (strcmp(name, "Json") == 0)     return XR_KIND_JSON;
    if (strcmp(name, "function") == 0) return XR_KIND_FUNCTION;
    if (strcmp(name, "Regex") == 0)    return XR_KIND_INSTANCE;
    if (strcmp(name, "BigInt") == 0)   return XR_KIND_INSTANCE;
    if (strcmp(name, "Channel") == 0)  return XR_KIND_CHANNEL;
    return XR_KIND_COUNT;
}

// Helper: get display name for XrTypeKind as Type.xxx
static const char *kind_to_type_member(XrTypeKind kind) {
    switch (kind) {
        case XR_KIND_INT:      return "Type.int";
        case XR_KIND_FLOAT:    return "Type.float";
        case XR_KIND_STRING:   return "Type.string";
        case XR_KIND_BOOL:     return "Type.bool";
        case XR_KIND_NULL:     return "Type.null";
        case XR_KIND_ARRAY:    return "Type.Array";
        case XR_KIND_MAP:      return "Type.Map";
        case XR_KIND_SET:      return "Type.Set";
        case XR_KIND_JSON:     return "Type.Json";
        case XR_KIND_FUNCTION: return "Type.function";
        case XR_KIND_CHANNEL:  return "Type.Channel";
        default:               return NULL;
    }
}

// Helper: collect Type.xxx member names from a match arm pattern
static void collect_matched_type_members(AstNode *pattern, XrTypeKind *kinds,
                                          int *count, int cap) {
    if (!pattern || *count >= cap) return;

    if (pattern->type == AST_PATTERN_LITERAL && pattern->as.pattern_literal.value) {
        AstNode *val = pattern->as.pattern_literal.value;
        if (val->type == AST_MEMBER_ACCESS && val->as.member_access.object &&
            val->as.member_access.object->type == AST_VARIABLE &&
            val->as.member_access.object->as.variable.name &&
            strcmp(val->as.member_access.object->as.variable.name, "Type") == 0) {
            XrTypeKind k = type_member_to_kind(val->as.member_access.name);
            if (k != XR_KIND_COUNT && *count < cap) {
                kinds[(*count)++] = k;
            }
        }
    } else if (pattern->type == AST_MEMBER_ACCESS && pattern->as.member_access.object &&
               pattern->as.member_access.object->type == AST_VARIABLE &&
               pattern->as.member_access.object->as.variable.name &&
               strcmp(pattern->as.member_access.object->as.variable.name, "Type") == 0) {
        XrTypeKind k = type_member_to_kind(pattern->as.member_access.name);
        if (k != XR_KIND_COUNT && *count < cap) {
            kinds[(*count)++] = k;
        }
    } else if (pattern->type == AST_PATTERN_MULTI) {
        PatternMultiNode *multi = &pattern->as.pattern_multi;
        for (int i = 0; i < multi->count; i++) {
            collect_matched_type_members(multi->patterns[i], kinds, count, cap);
        }
    }
}

// Helper: add enum member name to collection
static void add_enum_member(const char *member, const char ***names, int *count, int *cap) {
    if (!member) return;
    if (*count >= *cap) {
        *cap = *cap ? *cap * 2 : 8;
        *names = xr_realloc(*names, sizeof(const char*) * (*cap));
    }
    (*names)[(*count)++] = member;
}

// Helper: collect enum member names from a match arm pattern
static void collect_matched_enum_members(AstNode *pattern, const char ***names, 
                                          int *count, int *cap) {
    if (!pattern) return;
    
    if (pattern->type == AST_ENUM_ACCESS) {
        add_enum_member(pattern->as.enum_access.member_name, names, count, cap);
    } else if (pattern->type == AST_MEMBER_ACCESS) {
        add_enum_member(pattern->as.member_access.name, names, count, cap);
    } else if (pattern->type == AST_PATTERN_LITERAL && pattern->as.pattern_literal.value) {
        // Unwrap: AST_PATTERN_LITERAL wrapping AST_MEMBER_ACCESS or AST_ENUM_ACCESS
        collect_matched_enum_members(pattern->as.pattern_literal.value, names, count, cap);
    } else if (pattern->type == AST_PATTERN_MULTI) {
        PatternMultiNode *multi = &pattern->as.pattern_multi;
        for (int i = 0; i < multi->count; i++) {
            collect_matched_enum_members(multi->patterns[i], names, count, cap);
        }
    }
}

XrType *xa_visit_match_expr(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node) return xr_type_new_unknown();
    
    MatchExprNode *match = &node->as.match_expr;
    
    // Infer subject type
    XrType *subject_type = NULL;
    if (match->expr) {
        subject_type = xa_visit_infer_expr(ctx, match->expr);
    }
    
    // Collect arm body types and union them
    if (match->arm_count == 0) {
        return xr_type_new_never();
    }
    
    XrType *result = NULL;
    bool has_wildcard = false;
    
    for (int i = 0; i < match->arm_count; i++) {
        AstNode *arm = match->arms[i];
        if (!arm || arm->type != AST_MATCH_ARM) continue;
        
        MatchArmNode *arm_node = &arm->as.match_arm;
        
        // Check for wildcard pattern
        if (arm_node->pattern && arm_node->pattern->type == AST_PATTERN_WILDCARD) {
            has_wildcard = true;
        }
        
        // Register binding variable if pattern is a variable capture (e.g. n if (n < 0) => ...)
        bool has_binding = false;
        if (arm_node->pattern && arm_node->pattern->type == AST_PATTERN_LITERAL) {
            AstNode *pval = arm_node->pattern->as.pattern_literal.value;
            if (pval && pval->type == AST_VARIABLE) {
                has_binding = true;
                xa_analyzer_enter_scope(ctx->analyzer, XA_SCOPE_BLOCK, arm);
                XaSymbol *bind_sym = xa_symbol_new(pval->as.variable.name, XA_SYM_VARIABLE);
                bind_sym->location.line = pval->line;
                xa_scope_add_symbol(ctx->analyzer->current_scope, bind_sym);
                XaSymbolLinks *bind_links = xa_analyzer_get_links(ctx->analyzer, bind_sym);
                if (bind_links) {
                    bind_links->type = subject_type ? subject_type : xr_type_new_unknown();
                    bind_links->is_definitely_assigned = true;
                }
            }
        }
        
        // Infer guard if present
        if (arm_node->guard) {
            xa_visit_infer_expr(ctx, arm_node->guard);
        }
        
        // Infer body type
        XrType *arm_type = xr_type_new_unknown();
        if (arm_node->body) {
            arm_type = xa_visit_infer_expr(ctx, arm_node->body);
        }
        
        if (has_binding) {
            xa_analyzer_exit_scope(ctx->analyzer);
        }
        
        if (!result) {
            result = arm_type;
        } else if (!xr_type_equals(result, arm_type)) {
            result = xr_type_union(result, arm_type);
        }
    }
    
    // Exhaustiveness check for enum types
    // Resolve subject to enum type if possible:
    // 1. subject_type is already XR_KIND_ENUM
    // 2. subject is a variable whose declared_type resolves to an enum
    //    (parser creates XR_KIND_CLASS for "Color", need to check if it's actually an enum)
    if (!has_wildcard && !XR_TYPE_IS_ENUM(subject_type) && match->expr && match->expr->type == AST_VARIABLE) {
        const char *var_name = match->expr->as.variable.name;
        XaSymbol *var_sym = xa_scope_lookup(ctx->analyzer->current_scope, var_name);
        if (var_sym) {
            XaSymbolLinks *var_links = xa_analyzer_get_links(ctx->analyzer, var_sym);
            XrType *dt = var_links ? var_links->declared_type : NULL;
            if (dt) {
                if (XR_TYPE_IS_ENUM(dt)) {
                    subject_type = dt;
                } else if (XR_TYPE_IS_CLASS(dt) && dt->instance.class_name) {
                    // Parser resolves enum names as XR_KIND_CLASS; check symbol table
                    XaSymbol *maybe_enum = xa_scope_lookup(ctx->analyzer->current_scope, dt->instance.class_name);
                    if (maybe_enum && maybe_enum->kind == XA_SYM_ENUM) {
                        XaSymbolLinks *el = xa_analyzer_get_links(ctx->analyzer, maybe_enum);
                        if (el && el->type && XR_TYPE_IS_ENUM(el->type)) {
                            subject_type = el->type;
                        }
                    }
                }
            }
        }
    }
    if (subject_type && XR_TYPE_IS_ENUM(subject_type) && !has_wildcard) {
        const char *enum_name = subject_type->enum_type.enum_name;
        if (enum_name) {
            XaSymbol *enum_sym = xa_scope_lookup(ctx->analyzer->current_scope, enum_name);
            if (enum_sym && enum_sym->kind == XA_SYM_ENUM) {
                XaSymbolLinks *enum_links = xa_analyzer_get_links(ctx->analyzer, enum_sym);
                
                if (enum_links && enum_links->enum_member_count > 0) {
                    // Collect matched members from all arms
                    const char **matched = NULL;
                    int matched_count = 0, matched_cap = 0;
                    
                    for (int i = 0; i < match->arm_count; i++) {
                        AstNode *arm = match->arms[i];
                        if (!arm || arm->type != AST_MATCH_ARM) continue;
                        collect_matched_enum_members(arm->as.match_arm.pattern,
                                                     &matched, &matched_count, &matched_cap);
                    }
                    
                    // Check which enum members are missing
                    for (int i = 0; i < enum_links->enum_member_count; i++) {
                        const char *member_name = enum_links->enum_member_names[i];
                        if (!member_name) continue;
                        
                        bool found = false;
                        for (int j = 0; j < matched_count; j++) {
                            if (strcmp(matched[j], member_name) == 0) {
                                found = true;
                                break;
                            }
                        }
                        
                        if (!found) {
                            XrLocation loc = { .file = ctx->file_path, .line = node->line, .column = node->column };
                            char msg[256];
                            snprintf(msg, sizeof(msg),
                                "Non-exhaustive match: missing '%s.%s'", enum_name, member_name);
                            xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                XR_ERR_ANALYZE_TYPE_MISMATCH, msg, &loc);
                        }
                    }
                    
                    if (matched) xr_free(matched);
                }
            }
        }
    }
    
    // Exhaustiveness check for typeof match on union/nullable types
    // Detects: match typeof(x) { Type.int => ..., Type.string => ... }
    if (!has_wildcard && match->expr) {
        const char *typeof_var = get_typeof_arg_name(match->expr);
        if (typeof_var) {
            // Look up the static type of the variable
            XaSymbol *var_sym = xa_scope_lookup(ctx->analyzer->current_scope, typeof_var);
            XrType *var_type = NULL;
            if (var_sym) {
                XaSymbolLinks *var_links = xa_analyzer_get_links(ctx->analyzer, var_sym);
                if (var_links) var_type = var_links->declared_type ? var_links->declared_type : var_links->type;
            }
            
            if (var_type) {
                // Build expected type kind set from the variable's static type
                XrTypeKind expected[XR_UNION_MAX_MEMBERS + 1];
                int expected_count = 0;
                
                if (XR_TYPE_IS_UNION(var_type)) {
                    for (int i = 0; i < var_type->union_type.member_count && expected_count < XR_UNION_MAX_MEMBERS; i++) {
                        XrType *m = var_type->union_type.members[i];
                        if (m) expected[expected_count++] = m->kind;
                    }
                } else if (var_type->is_nullable) {
                    // T? means we need T and null
                    XrType *base = xr_type_non_nullable(var_type);
                    if (base) expected[expected_count++] = base->kind;
                    expected[expected_count++] = XR_KIND_NULL;
                }
                
                if (expected_count > 0) {
                    // Collect matched Type.xxx kinds from arms
                    XrTypeKind matched_kinds[32];
                    int matched_count = 0;
                    
                    for (int i = 0; i < match->arm_count; i++) {
                        AstNode *arm = match->arms[i];
                        if (!arm || arm->type != AST_MATCH_ARM) continue;
                        collect_matched_type_members(arm->as.match_arm.pattern,
                                                     matched_kinds, &matched_count, 32);
                    }
                    
                    // Check which expected types are missing
                    for (int i = 0; i < expected_count; i++) {
                        bool found = false;
                        for (int j = 0; j < matched_count; j++) {
                            if (matched_kinds[j] == expected[i]) {
                                found = true;
                                break;
                            }
                        }
                        if (!found) {
                            const char *missing = kind_to_type_member(expected[i]);
                            if (missing) {
                                XrLocation loc = { .file = ctx->file_path, .line = node->line, .column = node->column };
                                char msg[256];
                                snprintf(msg, sizeof(msg),
                                    "Non-exhaustive match: missing '%s' for type '%s'",
                                    missing, xr_type_to_string(var_type));
                                xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                    XR_ERR_ANALYZE_TYPE_MISMATCH, msg, &loc);
                            }
                        }
                    }
                }
            }
        }
    }
    
    return result ? result : xr_type_new_unknown();
}

XrType *xa_visit_function_expr(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node) return xr_type_new_function(NULL, 0, xr_type_new_unknown(), false);
    
    FunctionDeclNode *fn = &node->as.function_expr;
    
    // Check if has rest parameter
    bool has_rest = false;
    for (int i = 0; i < fn->param_count; i++) {
        if (fn->params[i] && fn->params[i]->is_rest) {
            has_rest = true;
            break;
        }
    }
    
    // Extract expected function type for bidirectional inference
    XrType *expected_fn = NULL;
    if (ctx->expected_type && XR_TYPE_IS_FUNCTION(ctx->expected_type)) {
        expected_fn = ctx->expected_type;
    }
    
    XrType **param_types = NULL;
    if (fn->param_count > 0) {
        param_types = xr_malloc(sizeof(XrType*) * fn->param_count);
        for (int i = 0; i < fn->param_count; i++) {
            XrParamNode *p = fn->params[i];
            // Check for explicit type annotation first
            if (p && p->type) {
                param_types[i] = (XrType*)p->type;
            }
            // Use expected function type (bidirectional inference)
            else if (expected_fn && i < expected_fn->function.param_count &&
                     expected_fn->function.param_types[i]) {
                param_types[i] = expected_fn->function.param_types[i];
            }
            // Use generic inference from callback context
            else if (i == 0 && ctx->callback_accumulator_type) {
                param_types[i] = ctx->callback_accumulator_type;
            } else if (i == 0 && ctx->callback_element_type) {
                param_types[i] = ctx->callback_element_type;
            } else if (i == 1 && ctx->callback_accumulator_type && ctx->callback_element_type) {
                param_types[i] = ctx->callback_element_type;
            } else if (i == 1 && ctx->callback_index_type) {
                param_types[i] = ctx->callback_index_type;
            } else if (i == 2 && ctx->callback_accumulator_type && ctx->callback_index_type) {
                param_types[i] = ctx->callback_index_type;
            } else if (i == 2 && ctx->callback_array_type) {
                param_types[i] = ctx->callback_array_type;
            } else if (i == 3 && ctx->callback_array_type) {
                param_types[i] = ctx->callback_array_type;
            } else {
                // Cannot infer parameter type — report error
                param_types[i] = xr_type_new_unknown();
                if (p && p->name && !p->is_rest) {
                    XrLocation loc = { .file = ctx->file_path, .line = p->line, .column = p->column };
                    char msg[256];
                    snprintf(msg, sizeof(msg),
                        "Parameter '%s' of anonymous function cannot be inferred, add explicit type annotation",
                        p->name);
                    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                        XR_ERR_ANALYZE_MISSING_TYPE, msg, &loc);
                }
            }
        }
    }
    
    // Use expected return type if not explicitly declared
    XrType *return_type = fn->return_type ? (XrType*)fn->return_type : xr_type_new_unknown();
    if (XR_TYPE_IS_UNKNOWN(return_type) && expected_fn && expected_fn->function.return_type) {
        return_type = expected_fn->function.return_type;
    }
    
    // Infer return type by entering function scope, registering params, and
    // analyzing body — same as named functions in Pass 3.
    if (XR_TYPE_IS_UNKNOWN(return_type) && fn->body) {
        // Save outer function's return type collection state
        XrType **saved_return_types = ctx->return_types;
        int saved_return_count = ctx->return_type_count;
        int saved_return_cap = ctx->return_type_capacity;
        ctx->return_types = NULL;
        ctx->return_type_count = 0;
        ctx->return_type_capacity = 0;
        
        // Enter function scope
        xa_analyzer_enter_scope(ctx->analyzer, XA_SCOPE_FUNCTION, node);
        
        // Register parameters in scope (with their inferred types)
        for (int i = 0; i < fn->param_count; i++) {
            XrParamNode *p = fn->params[i];
            if (p && p->name) {
                XaSymbol *param_sym = xa_symbol_new(p->name, XA_SYM_PARAMETER);
                param_sym->location.line = p->line > 0 ? p->line : node->line;
                xa_scope_add_symbol(ctx->analyzer->current_scope, param_sym);
                XaSymbolLinks *pl = xa_analyzer_get_links(ctx->analyzer, param_sym);
                pl->type = param_types ? param_types[i] : xr_type_new_unknown();
                pl->is_definitely_assigned = true;
            }
        }
        
        // Set expected return type for bidirectional inference on return stmts.
        // If contextual type provides a return type, use it for checking.
        // Otherwise, reset to NULL so the outer function's expected_return_type
        // doesn't leak into this closure (e.g. void from enclosing fn).
        XrType *saved_expected_ret = ctx->expected_return_type;
        if (expected_fn && expected_fn->function.return_type &&
            !XR_TYPE_IS_UNKNOWN(expected_fn->function.return_type)) {
            ctx->expected_return_type = expected_fn->function.return_type;
        } else if (fn->return_type) {
            ctx->expected_return_type = (XrType *)fn->return_type;
        } else {
            ctx->expected_return_type = NULL;
        }
        
        // Unified body visitor: idempotent collect + direct traversal
        xa_visit_function_body_unified(ctx, fn->body);
        
        ctx->expected_return_type = saved_expected_ret;
        
        // Compute unified return type from all collected return statements
        XrType *inferred_ret = xa_infer_compute_return_type(ctx);
        if (inferred_ret && !XR_TYPE_IS_UNKNOWN(inferred_ret)) {
            return_type = inferred_ret;
        }
        
        xa_analyzer_exit_scope(ctx->analyzer);
        
        // Restore outer function's return type state
        if (ctx->return_types) xr_free(ctx->return_types);
        ctx->return_types = saved_return_types;
        ctx->return_type_count = saved_return_count;
        ctx->return_type_capacity = saved_return_cap;
    }
    
    // After full body analysis, report error if return type still unknown
    if (XR_TYPE_IS_UNKNOWN(return_type) && fn->body && xa_body_has_return_expr(fn->body)) {
        XrLocation loc = { .file = ctx->file_path, .line = node->line, .column = node->column };
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
            XR_ERR_ANALYZE_MISSING_TYPE,
            "Anonymous function returns a value but return type cannot be inferred, "
            "add explicit type annotation", &loc);
    }
    
    XrType *result = xr_type_new_function(param_types, fn->param_count, return_type, has_rest);
    
    if (param_types) xr_free(param_types);
    return result;
}

/* ----------------------------------------------------------------------------
 * Coroutine Closure Capture Validation
 * Ensures coroutines don't capture non-shared variables unsafely.
 * Function arguments are deep-copied (safe), but closure captures need checking.
 * -------------------------------------------------------------------------- */
void check_closure_capture(XaInferContext *ctx, AstNode *node, int line) {
    if (!node) return;
    
    switch (node->type) {
        case AST_VARIABLE: {
            const char *name = node->as.variable.name;
            XaSymbol *sym = xa_scope_lookup(ctx->analyzer->current_scope, name);
            if (sym && sym->kind != XA_SYM_PARAMETER) {
                // Check if variable is shared, builtin, or a type (function/class)
                if (!sym->is_shared && !sym->is_builtin && 
                    sym->kind != XA_SYM_FUNCTION && sym->kind != XA_SYM_CLASS &&
                    sym->kind != XA_SYM_MODULE && sym->kind != XA_SYM_IMPORT) {
                    // Non-shared variable captured in coroutine closure
                    XrLocation loc = { .file = ctx->file_path, .line = line, .column = node->column };
                    char msg[512];
                    snprintf(msg, sizeof(msg), 
                        "Coroutine closure captured non-thread-safe variable '%s'\n"
                        "hint: Use one of the following:\n"
                        "  1. Pass through parameter: go worker(%s)  // automatic deep copy\n"
                        "  2. Declare as shared: shared let/const %s = ...",
                        name, name, name);
                    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_CLOSURE_CAPTURE, msg, &loc);
                }
            }
            break;
        }
        case AST_BINARY_ADD: case AST_BINARY_SUB: case AST_BINARY_MUL:
        case AST_BINARY_DIV: case AST_BINARY_MOD: case AST_BINARY_EQ:
        case AST_BINARY_NE: case AST_BINARY_LT: case AST_BINARY_LE:
        case AST_BINARY_GT: case AST_BINARY_GE: case AST_BINARY_AND:
        case AST_BINARY_OR:
            check_closure_capture(ctx, node->as.binary.left, line);
            check_closure_capture(ctx, node->as.binary.right, line);
            break;
        case AST_UNARY_NEG: case AST_UNARY_NOT: case AST_UNARY_BNOT:
            check_closure_capture(ctx, node->as.unary.operand, line);
            break;
        case AST_CALL_EXPR:
            // Only check callee, not arguments (arguments are deep-copied, safe)
            check_closure_capture(ctx, node->as.call_expr.callee, line);
            break;
        case AST_MEMBER_ACCESS:
            check_closure_capture(ctx, node->as.member_access.object, line);
            break;
        case AST_FUNCTION_EXPR:
            // Nested closure - check its body for captured variables
            if (node->as.function_expr.body) {
                check_closure_capture(ctx, node->as.function_expr.body, line);
            }
            break;
        case AST_BLOCK:
            for (int i = 0; i < node->as.block.count; i++) {
                check_closure_capture(ctx, node->as.block.statements[i], line);
            }
            break;
        case AST_VAR_DECL:
        case AST_CONST_DECL:
            // Check initializer
            if (node->as.var_decl.initializer) {
                check_closure_capture(ctx, node->as.var_decl.initializer, line);
            }
            break;
        case AST_ASSIGNMENT:
            check_closure_capture(ctx, node->as.assignment.value, line);
            break;
        case AST_IF_STMT:
            check_closure_capture(ctx, node->as.if_stmt.condition, line);
            check_closure_capture(ctx, node->as.if_stmt.then_branch, line);
            if (node->as.if_stmt.else_branch) {
                check_closure_capture(ctx, node->as.if_stmt.else_branch, line);
            }
            break;
        case AST_WHILE_STMT:
            check_closure_capture(ctx, node->as.while_stmt.condition, line);
            check_closure_capture(ctx, node->as.while_stmt.body, line);
            break;
        case AST_FOR_STMT:
            if (node->as.for_stmt.initializer) {
                check_closure_capture(ctx, node->as.for_stmt.initializer, line);
            }
            if (node->as.for_stmt.condition) {
                check_closure_capture(ctx, node->as.for_stmt.condition, line);
            }
            if (node->as.for_stmt.increment) {
                check_closure_capture(ctx, node->as.for_stmt.increment, line);
            }
            check_closure_capture(ctx, node->as.for_stmt.body, line);
            break;
        case AST_RETURN_STMT:
            for (int i = 0; i < node->as.return_stmt.value_count; i++) {
                check_closure_capture(ctx, node->as.return_stmt.values[i], line);
            }
            break;
        default:
            break;
    }
}

// Check coroutine expression for closure capture issues
// go fn(args) - args are passed by deep copy (safe)
// go { ... }  - closure captures need checking
void check_coro_capture(XaInferContext *ctx, AstNode *node, int line) {
    if (!node) return;
    
    // go fn(args) - function call with arguments passed by value (safe)
    if (node->type == AST_CALL_EXPR) {
        // Don't check arguments - they are deep copied
        // Only check if callee itself is a closure that captures variables
        CallExprNode *call = &node->as.call_expr;
        if (call->callee && call->callee->type == AST_FUNCTION_EXPR) {
            // go (fn() { ... })(args) - check the closure body
            check_closure_capture(ctx, call->callee->as.function_expr.body, line);
        }
        return;
    }
    
    // go { ... } - block or closure, check for captured variables
    if (node->type == AST_FUNCTION_EXPR) {
        if (node->as.function_expr.body) {
            check_closure_capture(ctx, node->as.function_expr.body, line);
        }
        return;
    }
    
    // go block { ... }
    if (node->type == AST_BLOCK) {
        check_closure_capture(ctx, node, line);
        return;
    }
}

XrType *xa_visit_go_expr(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node) return xr_type_new_task(xr_type_new_unknown());
    
    GoExprNode *go = &node->as.go_expr;
    
    // Infer the type of the expression being spawned
    XrType *result_type = xr_type_new_void();
    if (go->expr) {
        XrType *expr_type = xa_visit_infer_expr(ctx, go->expr);
        // If spawning a function call, get its return type
        if (XR_TYPE_IS_FUNCTION(expr_type) && expr_type->function.return_type) {
            result_type = expr_type->function.return_type;
        } else if (!XR_TYPE_IS_FUNCTION(expr_type)) {
            // Direct expression result
            result_type = expr_type;
        }
        
        // Check coroutine closure capture rules
        // Coroutine closures can only capture shared variables
        // Regular variables must be passed as arguments (deep copy)
        check_coro_capture(ctx, go->expr, node->line);
    }
    
    // go expr returns Task<T> where T is the result type
    return xr_type_new_task(result_type);
}

XrType *xa_visit_await_expr(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node) return xr_type_new_unknown();
    
    AwaitExprNode *await = &node->as.await_expr;
    
    // Infer the type of the awaited expression
    if (await->expr) {
        XrType *expr_type = xa_visit_infer_expr(ctx, await->expr);
        
        // P2-2: await all/any/anySuccess operates on Array<Task<T>> → Array<T> / T
        if (await->is_all || await->is_any || await->is_any_success) {
            // These forms take an array of tasks; extract element type
            if (expr_type && XR_TYPE_IS_ARRAY(expr_type)) {
                XrType *elem = expr_type->container.element_type;
                XrType *result_elem = xr_type_new_unknown();
                if (xr_type_is_named_class(elem, "Task") && elem->instance.type_arg_count > 0) {
                    result_elem = elem->instance.type_args[0];
                }
                if (await->is_all) {
                    return xr_type_new_array(result_elem);
                }
                // await any / anySuccess returns single element
                return result_elem;
            }
            return xr_type_new_unknown();
        }
        
        // Single await: extract result type from Task<T>
        if (xr_type_is_named_class(expr_type, "Task")) {
            XrType *result_type = (expr_type->instance.type_arg_count > 0) ? expr_type->instance.type_args[0] : NULL;
            return result_type ? result_type : xr_type_new_unknown();
        }
        
        // await [arr] is syntactic sugar for await.all — treat array as Task array
        if (XR_TYPE_IS_ARRAY(expr_type)) {
            XrType *elem = expr_type->container.element_type;
            XrType *result_elem = xr_type_new_unknown();
            if (elem && xr_type_is_named_class(elem, "Task") && elem->instance.type_arg_count > 0) {
                result_elem = elem->instance.type_args[0];
            }
            return xr_type_new_array(result_elem);
        }
        
        // If not a Task, report error (skip for unknown type which means inference failed)
        if (expr_type && !XR_TYPE_IS_UNKNOWN(expr_type)) {
            XrLocation loc = { .file = ctx->file_path, .line = node->line, .column = node->column };
            xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_AWAIT_TYPE,
                "await expects a Task type", &loc);
        }
    }
    
    return xr_type_new_unknown();
}

/*
 * Visit move expression: move var
 * Compile-time checks:
 *   - must be a variable (enforced by parser)
 *   - cannot move const value
 *   - cannot move Channel (thread-safe, shared by incref)
 *   - cannot move value types (int/float/bool/string — no heap object)
 */
XrType *xa_visit_move_expr(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node) return xr_type_new_unknown();
    
    MoveExprNode *move = &node->as.move_expr;
    AstNode *inner = move->expr;
    if (!inner) return xr_type_new_unknown();
    
    // Infer type of the variable being moved
    XrType *var_type = xa_visit_infer_expr(ctx, inner);
    
    XrLocation loc = { .file = ctx->file_path, .line = node->line, .column = node->column };
    
    // Check: variable must exist and be a let variable (not const)
    if (inner->type == AST_VARIABLE) {
        const char *name = inner->as.variable.name;
        XaSymbol *sym = xa_scope_lookup(ctx->analyzer->current_scope, name);
        if (sym && sym->is_const) {
            char msg[128];
            snprintf(msg, sizeof(msg), "cannot move const value '%s'", name);
            xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_ARG_TYPE, msg, &loc);
        }
    }
    
    // Check: cannot move Channel types
    if (var_type && var_type->kind == XR_KIND_CHANNEL) {
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_ARG_TYPE,
            "cannot move Channel (thread-safe, shared by reference)", &loc);
    }
    
    // Check: cannot move value types (no heap object to transfer)
    if (var_type && xr_kind_is_primitive(var_type->kind)) {
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_ARG_TYPE,
            "move is not meaningful for value type", &loc);
    }
    
    // move expr has the same type as the inner expression
    return var_type ? var_type : xr_type_new_unknown();
}

