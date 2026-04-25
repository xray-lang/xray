/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xanalyzer_visitor_call.c - Pass 2 visitor for function/method calls
 *
 * KEY CONCEPT:
 *   xa_visit_call() is the largest single visitor in the analyzer
 *   (~330 lines): it handles argument count and type checking, generic
 *   type-argument validation, type-parameter inference for generic
 *   functions, and callback-type inference for container methods
 *   (map / filter / reduce / forEach / find / every / some).
 *
 *   Phase 2.3 split it out of xanalyzer_visitor_expr.c so that the
 *   expression-shaped visitor file stayed under the 1500-line cohesion
 *   target. The implementation is unchanged from before the split.
 */

#include "xanalyzer_visitor_internal.h"
#include "../../base/xchecks.h"

/* ----------------------------------------------------------------------------
 * Function Call Type Inference
 * Handles: argument count/type checking, generic type argument validation,
 * type parameter inference for generic functions, and callback type inference
 * for container methods (map, filter, reduce, etc.)
 * -------------------------------------------------------------------------- */
XrType *xa_visit_call(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node) return xr_type_new_unknown(NULL);

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
                    return xr_type_new_instance(ctx->analyzer->isolate, links->class_info);
                }
            }
        }
        return xr_type_new_unknown(NULL);
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
                    return xr_type_new_instance(ctx->analyzer->isolate, links->class_info);
                }
            }
        }
        return xr_type_new_unknown(NULL);
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
        return xr_type_new_unknown(NULL);
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
            ctx->callback_index_type = xr_type_new_int(NULL);
        }
        // reduce: fn(acc, item, index, array) => acc
        else if (strcmp(method_name, "reduce") == 0) {
            ctx->callback_element_type = container_elem_type;
            ctx->callback_index_type = xr_type_new_int(NULL);
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

            // X-01 Phase 2.4b: side-table read.
            XrType *arg_type = xa_analyzer_get_node_type(ctx->analyzer,
                                                          call->arguments[i]);
            if (!arg_type || XR_TYPE_IS_UNKNOWN(arg_type))
                continue;

            XrType **slot = &fn_links->inferred_param_types[i];
            if (!*slot) {
                *slot = arg_type;  // First observation
            } else if (!xr_type_equals(*slot, arg_type)) {
                // If types differ only in nullability, widen to nullable
                XrType *a = xr_type_non_nullable(ctx->analyzer->isolate, *slot);
                XrType *b = xr_type_non_nullable(ctx->analyzer->isolate, arg_type);
                if (a && b && xr_type_equals(a, b)) {
                    *slot = xr_type_make_nullable(ctx->analyzer->isolate, a);
                } else {
                    *slot = xr_type_new_unknown(NULL);  // Incompatible callers
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
            // X-01 Phase 2.4b: side-table read.
            XrType *cb_type = xa_analyzer_get_node_type(ctx->analyzer,
                                                         call->arguments[0]);
            if (cb_type && XR_TYPE_IS_FUNCTION(cb_type) && cb_type->function.return_type &&
                !XR_TYPE_IS_UNKNOWN(cb_type->function.return_type)) {
                return_type = xr_type_new_array(ctx->analyzer->isolate, cb_type->function.return_type);
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
            return_type = xr_type_make_nullable(ctx->analyzer->isolate, xr_type_copy(ctx->analyzer->isolate, container_elem_type));
        } else if (strcmp(method_name, "findIndex") == 0) {
            return_type = xr_type_new_int(NULL);
        } else if (strcmp(method_name, "every") == 0 || strcmp(method_name, "some") == 0) {
            return_type = xr_type_new_bool(NULL);
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
                                return_type = xr_type_substitute(ctx->analyzer->isolate, return_type, class_param_names,
                                    callee_obj_type->instance.type_args, callee_obj_type->instance.type_arg_count);
                                xr_free(class_param_names);
                            }
                        }
                    }
                }
            }
        }
    }

    return return_type ? return_type : xr_type_new_unknown(NULL);
}
