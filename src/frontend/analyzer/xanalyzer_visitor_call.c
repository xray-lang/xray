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
 *   This file holds the call-shaped subset of the analyzer visitor
 *   (call expression inference, generic substitution, callback-type
 *   inference for container methods).
 */

#include "xanalyzer_visitor_internal.h"
#include "xtype_ref_resolve.h"
#include "../../base/xchecks.h"

static bool xa_object_literal_bool_field(AstNode *node, const char *field_name, bool *out_value) {
    if (!node || node->type != AST_OBJECT_LITERAL || !field_name || !out_value)
        return false;
    ObjectLiteralNode *obj = &node->as.object_literal;
    for (int i = 0; i < obj->count; i++) {
        if (obj->computed && obj->computed[i])
            continue;
        AstNode *key = obj->keys ? obj->keys[i] : NULL;
        AstNode *value = obj->values ? obj->values[i] : NULL;
        if (!key || key->type != AST_LITERAL_STRING || !value)
            continue;
        if (strcmp(key->as.literal.raw_value.string_val, field_name) != 0)
            continue;
        if (value->type == AST_LITERAL_TRUE) {
            *out_value = true;
            return true;
        }
        if (value->type == AST_LITERAL_FALSE) {
            *out_value = false;
            return true;
        }
    }
    return false;
}

static bool xa_is_module_call(CallExprNode *call, const char *module_name, const char *func_name) {
    if (!call || !module_name || !func_name || !call->callee ||
        call->callee->type != AST_MEMBER_ACCESS)
        return false;
    MemberAccessNode *ma = &call->callee->as.member_access;
    if (!ma->name || strcmp(ma->name, func_name) != 0 || !ma->object ||
        ma->object->type != AST_VARIABLE)
        return false;
    return strcmp(ma->object->as.variable.name, module_name) == 0;
}

static XrType *xa_csv_parse_return_type(XaInferContext *ctx, CallExprNode *call) {
    if (!xa_is_module_call(call, "csv", "parse"))
        return NULL;
    XrType *row_array =
        xr_type_new_array(ctx->analyzer->isolate, xr_type_new_string(ctx->analyzer->isolate));
    XrType *rows_array = xr_type_new_array(ctx->analyzer->isolate, row_array);
    if (call->arg_count < 2 || !call->arguments[1])
        return rows_array;
    bool header = false;
    if (xa_object_literal_bool_field(call->arguments[1], "header", &header))
        return header ? xr_type_new_array(ctx->analyzer->isolate,
                                          xr_type_new_json(ctx->analyzer->isolate))
                      : rows_array;
    return NULL;
}

/* ----------------------------------------------------------------------------
 * Function Call Type Inference
 * Handles: argument count/type checking, generic type argument validation,
 * type parameter inference for generic functions, and callback type inference
 * for container methods (map, filter, reduce, etc.)
 * -------------------------------------------------------------------------- */
XrType *xa_visit_call(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node)
        return xr_type_new_unknown(NULL);

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
                XaIncrementalCtx *incr = (XaIncrementalCtx *) ctx->analyzer->incremental;
                xa_dep_add(incr, ctx->current_function->id, fn_sym->id, XA_DEP_CALL);
            }
        }
    }

    /* Builtin heap types require 'new': Map(), Array(), Set(), Bytes(),
     * Channel(), StringBuilder(), WeakMap(), WeakSet() without 'new' is
     * an error — use 'new T()' instead. */
    if (call->callee && call->callee->type == AST_VARIABLE) {
        const char *cname = call->callee->as.variable.name;
        if (cname) {
            static const char *const heap_types[] = {"Map",     "Array",   "Set",
                                                     "Bytes",   "Channel", "StringBuilder",
                                                     "WeakMap", "WeakSet", NULL};
            for (const char *const *p = heap_types; *p; p++) {
                if (strcmp(cname, *p) == 0) {
                    XrLocation loc = {
                        .file = ctx->file_path, .line = node->line, .column = node->column};
                    char msg[128];
                    snprintf(msg, sizeof(msg), "Use 'new %s(...)' to construct %s", cname, cname);
                    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                               XR_ERR_ANALYZE_NOT_CALLABLE, msg, &loc);
                    break;
                }
            }
        }
    }

    // Check generic type argument count and constraints
    if (call->type_arg_count > 0 && fn_links) {
        int expected_count = xa_symbol_links_get_type_param_count(fn_links);

        // Check count matches
        if (call->type_arg_count != expected_count) {
            XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
            char msg[256];
            const char *fn_name = call->callee && call->callee->type == AST_VARIABLE
                                      ? call->callee->as.variable.name
                                      : "function";
            snprintf(msg, sizeof(msg),
                     "Generic function '%s' expects %d type argument(s), but got %d", fn_name,
                     expected_count, call->type_arg_count);
            xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                       XR_ERR_ANALYZE_GENERIC_COUNT, msg, &loc);
        }

        // Check constraints — every constraint in the intersection list must hold.
        for (int i = 0; i < call->type_arg_count && i < expected_count; i++) {
            // Use analyzer-aware resolver so user class type-args carry their
            // superclass chain — required for `<T: BaseClass>` upper bounds.
            XrType *type_arg = call->type_args[i]
                                   ? xr_tref_resolve_in_analyzer(ctx->analyzer, call->type_args[i])
                                   : NULL;

            int constraint_count = 0;
            XrType **constraints =
                xa_symbol_links_get_type_param_constraints(fn_links, i, &constraint_count);

            if (!type_arg || constraint_count == 0)
                continue;

            for (int j = 0; j < constraint_count; j++) {
                XrType *constraint = constraints[j];
                if (constraint && !xr_type_satisfies_constraint(type_arg, constraint)) {
                    XrLocation loc = {
                        .file = ctx->file_path, .line = node->line, .column = node->column};
                    const char *param_name = xa_symbol_links_get_type_param_name(fn_links, i);
                    char msg[256];
                    snprintf(msg, sizeof(msg),
                             "Type '%s' does not satisfy constraint '%s' for type parameter '%s'",
                             xr_type_to_string(type_arg), xr_type_to_string(constraint),
                             param_name ? param_name : "?");
                    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                               XR_ERR_ANALYZE_GENERIC_CONSTRAINT, msg, &loc);
                }
            }
        }
    }

    // Recognize Json.decode<T>(data): compiler-generated typed decode
    if (call->callee && call->callee->type == AST_MEMBER_ACCESS && call->type_arg_count == 1) {
        MemberAccessNode *ma = &call->callee->as.member_access;
        if (ma->name && strcmp(ma->name, "decode") == 0 && ma->object &&
            ma->object->type == AST_VARIABLE && strcmp(ma->object->as.variable.name, "Json") == 0) {
            XrType *target_type = call->type_args[0]
                                      ? xr_tref_resolve(ctx->analyzer->isolate, call->type_args[0])
                                      : NULL;

            // Resolve type alias to its underlying object type
            if (target_type && target_type->kind == XR_KIND_CLASS &&
                target_type->instance.class_name) {
                XaSymbol *alias_sym =
                    xa_scope_lookup(ctx->analyzer->current_scope, target_type->instance.class_name);
                if (alias_sym && alias_sym->kind == XA_SYM_TYPE_ALIAS && alias_sym->alias_type) {
                    target_type = alias_sym->alias_type;
                }
            }

            // Validate: target must be sealed Json type with known fields
            if (!target_type || !XR_TYPE_IS_JSON(target_type) || !target_type->object.is_sealed ||
                target_type->object.field_count == 0) {
                XrLocation loc = {
                    .file = ctx->file_path, .line = node->line, .column = node->column};
                xa_analyzer_add_diagnostic(
                    ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_GENERIC_CONSTRAINT,
                    "Json.decode<T>() requires T to be an object type (type alias with fields)",
                    &loc);
                return xr_type_new_unknown(NULL);
            }

            // Validate: exactly 1 argument
            if (call->arg_count != 1) {
                XrLocation loc = {
                    .file = ctx->file_path, .line = node->line, .column = node->column};
                xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                           XR_ERR_ANALYZE_ARG_TYPE,
                                           "Json.decode<T>() expects exactly 1 argument", &loc);
                return xr_type_new_unknown(NULL);
            }

            // Visit argument to ensure it's analyzed
            xa_visit_infer_expr(ctx, call->arguments[0]);

            // Return T? (decode can fail, returning null)
            XrType *result = xr_type_copy(ctx->analyzer->isolate, target_type);
            if (result)
                result->is_nullable = true;
            return result;
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
        } else if ((callee_obj_type->kind == XR_KIND_SET) &&
                   callee_obj_type->container.element_type) {
            container_elem_type = callee_obj_type->container.element_type;
        } else if (XR_TYPE_IS_MAP(callee_obj_type)) {
            // For Map, callback gets (value, key) or (key, value) depending on method
            if (method_name && strcmp(method_name, "forEach") == 0) {
                container_elem_type = callee_obj_type->map.value_type;
            }
        }
    }

    XrType *callee_type = xa_visit_infer_expr(ctx, call->callee);

    /* Resolve symbol_ids in non-lambda arguments before any early-return path.
     * Skip AST_FUNCTION_EXPR args: they require expected_type context from
     * the callee's parameter signature (set in the detailed loop below).
     * Visiting them eagerly without context triggers spurious E0365. */
    for (int i = 0; i < call->arg_count; i++) {
        if (call->arguments[i] && call->arguments[i]->type != AST_FUNCTION_EXPR)
            xa_visit_infer_expr(ctx, call->arguments[i]);
    }

    // Unknown callee type preserves error recovery after imprecise analysis.
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
        // Container method with callback: infer fn expr arg types even though
        // the method's own return type resolved to unknown (e.g. reduce).
        if (container_elem_type && method_name) {
            XrType *saved_elem = ctx->callback_element_type;
            XrType *saved_idx = ctx->callback_index_type;
            XrType *saved_acc = ctx->callback_accumulator_type;
            XrType *saved_arr = ctx->callback_array_type;
            ctx->callback_element_type = container_elem_type;
            ctx->callback_index_type = xr_type_new_int(NULL);
            if (strcmp(method_name, "reduce") == 0 && call->arg_count >= 2 && call->arguments[1]) {
                ctx->callback_accumulator_type = xa_visit_infer_expr(ctx, call->arguments[1]);
                ctx->callback_array_type = callee_obj_type;
            }
            for (int i = 0; i < call->arg_count; i++) {
                if (call->arguments[i])
                    xa_visit_infer_expr(ctx, call->arguments[i]);
            }
            ctx->callback_element_type = saved_elem;
            ctx->callback_index_type = saved_idx;
            ctx->callback_accumulator_type = saved_acc;
            ctx->callback_array_type = saved_arr;
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

    // Union of function types: method dispatch on union (e.g. shape.area()
    // where shape: Circle | Rect). Each union member resolves to a function
    // type; extract each return type and union them.
    if (XR_TYPE_IS_UNION(callee_type)) {
        XrType *ret_union = NULL;
        bool all_functions = true;
        for (int i = 0; i < callee_type->union_type.member_count; i++) {
            XrType *m = callee_type->union_type.members[i];
            if (!m || !XR_TYPE_IS_FUNCTION(m)) {
                all_functions = false;
                break;
            }
            XrType *rt = m->function.return_type;
            if (!rt)
                rt = xr_type_new_unknown(NULL);
            ret_union = ret_union ? xr_type_union(ctx->analyzer->isolate, ret_union, rt) : rt;
        }
        if (all_functions && ret_union)
            return ret_union;
    }

    // Check if callee is callable
    if (!XR_TYPE_IS_FUNCTION(callee_type)) {
        // Builtin method call: container.method() where member_access returned
        // the method's return type directly (e.g. arr.length() → int).
        // Accept primitive/container return types without warning.
        if (call->callee && call->callee->type == AST_MEMBER_ACCESS && callee_type &&
            !XR_TYPE_IS_UNKNOWN(callee_type)) {
            return callee_type;
        }
        XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_NOT_CALLABLE,
                                   "Value is not callable", &loc);
        return xr_type_new_unknown(NULL);
    }

    // Infer argument types
    int param_count = callee_type->function.param_count;
    bool is_variadic = callee_type->function.is_variadic;

    /* Spread expansion: walk arguments once, building a flat per-slot
     * view that splices each `...tuple` arg into its individual element
     * slots. With no spreads this is just call->arguments / call->arg_count. */
    int eff_count = 0;
    for (int i = 0; i < call->arg_count; i++) {
        AstNode *a = call->arguments[i];
        if (a && a->type == AST_SPREAD_EXPR) {
            XrType *src = xa_visit_infer_expr(ctx, a->as.spread_expr.expr);
            if (src && XR_TYPE_IS_TUPLE(src)) {
                eff_count += src->tuple.element_count;
            } else {
                XrLocation loc = {.file = ctx->file_path, .line = a->line, .column = a->column};
                char msg[160];
                snprintf(msg, sizeof(msg),
                         "Spread '...' argument must be a tuple of statically known arity, "
                         "got '%s'",
                         src ? xr_type_to_string(src) : "<unknown>");
                xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                           XR_ERR_ANALYZE_TYPE_MISMATCH, msg, &loc);
            }
        } else {
            eff_count++;
        }
    }
    int arg_count = eff_count;

    // Check argument count (use min_params for functions with default parameters)
    int min_params = callee_type->function.min_params;
    if (arg_count < min_params && !is_variadic) {
        XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
        char msg[128];
        if (min_params == param_count) {
            snprintf(msg, sizeof(msg), "Expected %d argument(s), but got %d", param_count,
                     arg_count);
        } else {
            snprintf(msg, sizeof(msg), "Expected %d to %d argument(s), but got %d", min_params,
                     param_count, arg_count);
        }
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_WRONG_ARG_COUNT,
                                   msg, &loc);
    } else if (arg_count > param_count && !is_variadic) {
        XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
        char msg[128];
        snprintf(msg, sizeof(msg), "Expected %d argument(s), but got %d", param_count, arg_count);
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_WRONG_ARG_COUNT,
                                   msg, &loc);
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

    /* Effective-slot iteration: a spread `...t` arg contributes one
     * slot per element of the source tuple. Each slot is checked
     * against the next parameter; non-tuple spread sources contribute
     * zero slots (the diagnostic was already emitted above). */
    int slot = 0;
    for (int i = 0; i < call->arg_count; i++) {
        AstNode *arg_node = call->arguments[i];
        if (!arg_node)
            continue;

        if (arg_node->type == AST_SPREAD_EXPR) {
            /* xa_visit_infer_expr was already called above for the
             * count check; re-querying gives the cached node type. */
            XrType *src = xa_analyzer_get_node_type(ctx->analyzer, arg_node->as.spread_expr.expr);
            if (!src) {
                XrType *saved_expected = ctx->expected_type;
                ctx->expected_type = NULL;
                src = xa_visit_infer_expr(ctx, arg_node->as.spread_expr.expr);
                ctx->expected_type = saved_expected;
            }
            if (!src || !XR_TYPE_IS_TUPLE(src))
                continue;
            for (int j = 0; j < src->tuple.element_count && slot < param_count; j++, slot++) {
                XrType *param_type = param_types ? param_types[slot] : NULL;
                if (!param_type || XR_TYPE_IS_UNKNOWN(param_type))
                    continue;
                XrType *arg_type = src->tuple.element_types[j];
                XrLocation loc = {
                    .file = ctx->file_path, .line = arg_node->line, .column = arg_node->column};
                bool null_err =
                    xa_check_null_safety(ctx->analyzer, param_type, arg_type, "Argument", &loc);
                if (!null_err && !xa_typecheck_assignable(param_type, arg_type) &&
                    !xr_is_json_coercion(param_type, arg_type)) {
                    char msg[256];
                    snprintf(msg, sizeof(msg),
                             "Argument %d (from spread): type '%s' is not assignable to "
                             "parameter type '%s'",
                             slot + 1, xr_type_to_string(arg_type), xr_type_to_string(param_type));
                    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                               XR_ERR_ANALYZE_ARG_TYPE, msg, &loc);
                }
            }
            continue;
        }

        if (slot >= param_count) {
            slot++;
            continue;
        }

        XrType *param_type = param_types ? param_types[slot] : NULL;
        XrType *saved_expected = ctx->expected_type;
        if (param_type && !XR_TYPE_IS_UNKNOWN(param_type)) {
            ctx->expected_type = param_type;
        }
        XrType *arg_type = xa_visit_infer_expr(ctx, arg_node);
        ctx->expected_type = saved_expected;

        if (param_type && !XR_TYPE_IS_UNKNOWN(param_type)) {
            XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
            bool null_err =
                xa_check_null_safety(ctx->analyzer, param_type, arg_type, "Argument", &loc);
            if (!null_err && !xa_typecheck_assignable(param_type, arg_type) &&
                !xr_is_json_coercion(param_type, arg_type)) {
                char msg[256];
                snprintf(msg, sizeof(msg),
                         "Argument %d: type '%s' is not assignable to parameter type '%s'",
                         slot + 1, xr_type_to_string(arg_type), xr_type_to_string(param_type));
                xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                           XR_ERR_ANALYZE_ARG_TYPE, msg, &loc);
            }
        }
        slot++;
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
            fn_links->inferred_param_types = xr_calloc(fn_links->param_count, sizeof(XrType *));
            fn_links->inferred_param_count = fn_links->param_count;
        }
        for (int i = 0; i < arg_count && i < fn_links->inferred_param_count; i++) {
            // Only propagate for unannotated params (declared_type is NULL or unknown)
            XrType *declared = (fn_links->param_types && i < fn_links->param_count)
                                   ? fn_links->param_types[i]
                                   : NULL;
            if (declared && !XR_TYPE_IS_UNKNOWN(declared))
                continue;  // explicitly typed

            // Read argument type from the analyzer side table.
            XrType *arg_type = xa_analyzer_get_node_type(ctx->analyzer, call->arguments[i]);
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
            // Read callback type from the analyzer side table.
            XrType *cb_type = xa_analyzer_get_node_type(ctx->analyzer, call->arguments[0]);
            if (cb_type && XR_TYPE_IS_FUNCTION(cb_type) && cb_type->function.return_type &&
                !XR_TYPE_IS_UNKNOWN(cb_type->function.return_type)) {
                return_type =
                    xr_type_new_array(ctx->analyzer->isolate, cb_type->function.return_type);
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
            return_type = xr_type_make_nullable(
                ctx->analyzer->isolate, xr_type_copy(ctx->analyzer->isolate, container_elem_type));
        } else if (strcmp(method_name, "findIndex") == 0) {
            return_type = xr_type_new_int(NULL);
        } else if (strcmp(method_name, "every") == 0 || strcmp(method_name, "some") == 0) {
            return_type = xr_type_new_bool(NULL);
        }
    }

    // Apply type substitution for generic function calls
    if (return_type && fn_links) {
        return_type =
            xa_substitute_generic_call(ctx, fn_links, callee_type, return_type, call, arg_count);
    }

    XrType *builtin_override = xa_csv_parse_return_type(ctx, call);
    if (builtin_override)
        return_type = builtin_override;

    // Apply type substitution for generic method calls: obj.method<T>()
    if (callee_obj_type && call->callee->type == AST_MEMBER_ACCESS) {
        MemberAccessNode *ma = &call->callee->as.member_access;

        // Look up method in class
        if (XR_TYPE_IS_INSTANCE(callee_obj_type) && callee_obj_type->instance.class_name) {
            XaSymbol *class_sym =
                xa_scope_lookup(ctx->analyzer->global_scope, callee_obj_type->instance.class_name);
            if (class_sym && class_sym->kind == XA_SYM_CLASS) {
                XaSymbolLinks *class_links = xa_analyzer_get_links(ctx->analyzer, class_sym);
                if (class_links && class_links->class_info) {
                    XaSymbol *method_sym =
                        xa_class_info_lookup_member(class_links->class_info, ma->name);
                    if (method_sym && method_sym->kind == XA_SYM_METHOD) {
                        XaSymbolLinks *method_links =
                            xa_analyzer_get_links(ctx->analyzer, method_sym);
                        if (method_links) {
                            // Apply method's own type parameters
                            return_type = xa_substitute_generic_call(ctx, method_links, callee_type,
                                                                     return_type, call, arg_count);

                            // Also apply class type parameters substitution
                            int class_type_param_count =
                                xa_symbol_links_get_type_param_count(class_links);
                            if (class_type_param_count > 0 &&
                                callee_obj_type->instance.type_arg_count > 0) {
                                const char **class_param_names =
                                    xr_malloc(sizeof(const char *) * class_type_param_count);
                                for (int i = 0; i < class_type_param_count; i++) {
                                    class_param_names[i] =
                                        xa_symbol_links_get_type_param_name(class_links, i);
                                }
                                return_type = xr_type_substitute(
                                    ctx->analyzer->isolate, return_type, class_param_names,
                                    callee_obj_type->instance.type_args,
                                    callee_obj_type->instance.type_arg_count);
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
