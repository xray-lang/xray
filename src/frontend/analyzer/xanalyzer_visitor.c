/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xanalyzer_visitor.c - AST visitor implementation
 *
 * KEY CONCEPT:
 *   Two-pass analysis: Pass 1 collects symbols (with hoisting for functions/classes),
 *   Pass 2 infers types and performs type checking.
 *
 */

#include "xanalyzer_visitor_internal.h"
#include "xtype_ref_resolve.h"
#include "../../base/xchecks.h"
#include "../../runtime/value/xstruct_layout.h"

// Forward declarations now in xanalyzer_visitor_internal.h

static const char *json_type_label_local(XrType *type) {
    if (type && XR_TYPE_IS_JSON(type) && type->object.type_name)
        return type->object.type_name;
    return xr_type_to_string(type);
}

static int json_field_index_local(XrType *type, const char *name) {
    if (!type || !XR_TYPE_IS_JSON(type) || !name || !type->object.field_names)
        return -1;
    for (int i = 0; i < type->object.field_count; i++) {
        if (type->object.field_names[i] && strcmp(type->object.field_names[i], name) == 0)
            return i;
    }
    return -1;
}

/*
 * Recursively convert XR_KIND_CLASS types whose class_name matches a declared
 * type parameter name into XR_KIND_TYPE_PARAM.  The parser has no knowledge of
 * generic scopes, so `T` in `fn add<T>(item: T)` is parsed as CLASS("T").
 * This fixup must run before the function/method type is finalised.
 */
// Cross-TU helper used by xa_visit_collect_class in
// xanalyzer_visitor_decl.c when registering generic-method types.
XrType *resolve_class_to_type_param(XrayIsolate *X, XrType *type, const char **tp_names,
                                    int tp_count) {
    if (!type || tp_count <= 0)
        return type;

    // Direct match: CLASS("T") → TYPE_PARAM("T")
    if (type->kind == XR_KIND_CLASS && type->instance.class_name) {
        for (int i = 0; i < tp_count; i++) {
            if (tp_names[i] && strcmp(type->instance.class_name, tp_names[i]) == 0)
                return xr_type_new_type_param(X, tp_names[i], i);
        }
    }

    // Recurse into containers
    if (type->kind == XR_KIND_ARRAY && type->container.element_type) {
        XrType *e =
            resolve_class_to_type_param(X, type->container.element_type, tp_names, tp_count);
        if (e != type->container.element_type)
            return xr_type_new_array(X, e);
    }
    if (type->kind == XR_KIND_MAP) {
        XrType *k = resolve_class_to_type_param(X, type->map.key_type, tp_names, tp_count);
        XrType *v = resolve_class_to_type_param(X, type->map.value_type, tp_names, tp_count);
        if (k != type->map.key_type || v != type->map.value_type)
            return xr_type_new_map(X, k, v);
    }
    if (XR_TYPE_IS_FUNCTION(type)) {
        bool changed = false;
        int pc = type->function.param_count;
        XrType **np = pc > 0 ? xr_malloc(sizeof(XrType *) * pc) : NULL;
        for (int i = 0; i < pc; i++) {
            np[i] =
                resolve_class_to_type_param(X, type->function.param_types[i], tp_names, tp_count);
            if (np[i] != type->function.param_types[i])
                changed = true;
        }
        XrType *ret =
            resolve_class_to_type_param(X, type->function.return_type, tp_names, tp_count);
        if (ret != type->function.return_type)
            changed = true;
        if (changed) {
            XrType *ft = xr_type_new_function(X, np, pc, ret, type->function.is_variadic);
            ft->function.min_params = type->function.min_params;
            xr_free(np);
            return ft;
        }
        if (np)
            xr_free(np);
    }
    // Generic instance: Box<T> etc.
    if (type->kind == XR_KIND_INSTANCE && type->instance.type_arg_count > 0) {
        bool changed = false;
        int ac = type->instance.type_arg_count;
        XrType **na = xr_malloc(sizeof(XrType *) * ac);
        for (int i = 0; i < ac; i++) {
            na[i] = resolve_class_to_type_param(X, type->instance.type_args[i], tp_names, tp_count);
            if (na[i] != type->instance.type_args[i])
                changed = true;
        }
        if (changed) {
            XrType *r = xr_type_new_generic_instance(X, type->instance.class_name,
                                                     type->instance.class_ref, na, ac);
            return r;
        }
        xr_free(na);
    }
    return type;
}

XR_FUNC void xa_set_function_type_params_from_ast(XaInferContext *ctx, XrType *fn_type,
                                                  XrGenericParam **type_params, int count) {
    if (!ctx || !ctx->analyzer || !fn_type || !XR_TYPE_IS_FUNCTION(fn_type) || count <= 0 ||
        !type_params)
        return;

    const char **names = xr_malloc(sizeof(const char *) * count);
    XrType ***constraint_lists = xr_malloc(sizeof(XrType **) * count);
    int *constraint_counts = xr_malloc(sizeof(int) * count);
    if (!names || !constraint_lists || !constraint_counts) {
        xr_free(names);
        xr_free(constraint_lists);
        xr_free(constraint_counts);
        return;
    }

    // Collect all names first so resolve_class_to_type_param can safely
    // read the full names array when checking constraints for early params.
    for (int i = 0; i < count; i++) {
        XrGenericParam *gp = type_params[i];
        names[i] = gp ? gp->name : NULL;
    }

    for (int i = 0; i < count; i++) {
        XrGenericParam *gp = type_params[i];
        int cn = gp ? gp->constraint_count : 0;
        constraint_counts[i] = cn;
        constraint_lists[i] = NULL;
        if (cn > 0 && gp && gp->constraints) {
            constraint_lists[i] = xr_malloc(sizeof(XrType *) * cn);
            if (constraint_lists[i]) {
                for (int j = 0; j < cn; j++) {
                    constraint_lists[i][j] =
                        gp->constraints[j]
                            ? xr_tref_resolve_in_analyzer(ctx->analyzer, gp->constraints[j])
                            : NULL;
                    constraint_lists[i][j] = resolve_class_to_type_param(
                        ctx->analyzer->isolate, constraint_lists[i][j], names, count);
                }
            } else {
                constraint_counts[i] = 0;
            }
        }
    }

    xr_type_set_function_type_params(ctx->analyzer->isolate, fn_type, names, constraint_lists,
                                     constraint_counts, count);

    for (int i = 0; i < count; i++) {
        if (constraint_lists[i])
            xr_free(constraint_lists[i]);
    }
    xr_free(names);
    xr_free(constraint_lists);
    xr_free(constraint_counts);
}

// Check null→T and T?→T assignment errors.
// Returns true if an error was emitted (caller should still call xr_type_assignable).
bool xa_check_null_safety(XaAnalyzer *analyzer, XrType *target, XrType *source,
                          const char *context_msg, XrLocation *loc) {
    if (!target || !source)
        return false;
    if (XR_TYPE_IS_UNKNOWN(target) || XR_TYPE_IS_UNKNOWN(source))
        return false;

    // Value types (structs) can never be null
    if (XR_TYPE_IS_NULL(source) && target->is_value_type) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "%s: cannot assign 'null' to value type '%s' (structs are non-nullable)",
                 context_msg, xr_type_to_string(target));
        xa_analyzer_add_diagnostic(analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_TYPE_MISMATCH, msg,
                                   loc);
        return true;
    }

    // Null-safety violations are always errors (xray is a strongly-typed language).
    XrDiagSeverity null_sev = XR_DIAG_SEV_ERROR;

    // null -> T. Reject unless the target is nullable (`T?`) or a
    // type whose value domain natively contains null (currently
    // Json). The `intrinsically_includes_null` helper centralises
    // that second predicate; see its doc comment for the rationale.
    if (XR_TYPE_IS_NULL(source) && !target->is_nullable &&
        !xr_type_intrinsically_includes_null(target)) {
        char msg[256];
        snprintf(msg, sizeof(msg), "%s: cannot assign 'null' to non-nullable type '%s'",
                 context_msg, xr_type_to_string(target));
        xa_analyzer_add_diagnostic(analyzer, null_sev, XR_ERR_ANALYZE_TYPE_MISMATCH, msg, loc);
        return true;
    }

    // T? → T (nullable source to non-nullable target, without narrowing)
    if (source->is_nullable && !target->is_nullable) {
        XrType *src_base = xr_type_non_nullable(analyzer->isolate, source);
        if (src_base && xr_type_assignable(target, src_base)) {
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "%s: cannot assign '%s' to '%s' without null check. "
                     "Use '?\?' default or '!' unwrap",
                     context_msg, xr_type_to_string(source), xr_type_to_string(target));
            xa_analyzer_add_diagnostic(analyzer, null_sev, XR_ERR_ANALYZE_TYPE_MISMATCH, msg, loc);
            return true;
        }
    }

    return false;
}

// Helper: recursively extract a type parameter from an argument type
// e.g., match(Array<T>, Array<int>, "T") -> int
//        match(Map<K,V>, Map<string,int>, "K") -> string
XrType *xa_infer_type_param_from_arg(XrType *param_type, XrType *arg_type, const char *tp_name,
                                     int depth) {
    if (!param_type || !arg_type || depth > 8)
        return NULL;

    // Direct match: T vs int -> T = int
    if ((param_type->kind == XR_KIND_TYPE_PARAM) && param_type->type_param.name &&
        strcmp(param_type->type_param.name, tp_name) == 0) {
        return arg_type;
    }

    // Array<T> vs Array<int> -> T = int
    if (XR_TYPE_IS_ARRAY(param_type) && XR_TYPE_IS_ARRAY(arg_type)) {
        return xa_infer_type_param_from_arg(param_type->container.element_type,
                                            arg_type->container.element_type, tp_name, depth + 1);
    }

    // Set<T> vs Set<int> -> T = int
    if ((param_type->kind == XR_KIND_SET) && (arg_type->kind == XR_KIND_SET)) {
        return xa_infer_type_param_from_arg(param_type->container.element_type,
                                            arg_type->container.element_type, tp_name, depth + 1);
    }

    // Map<K, V> vs Map<string, int> -> K = string, V = int
    if (XR_TYPE_IS_MAP(param_type) && XR_TYPE_IS_MAP(arg_type)) {
        XrType *from_key = xa_infer_type_param_from_arg(param_type->map.key_type,
                                                        arg_type->map.key_type, tp_name, depth + 1);
        if (from_key)
            return from_key;
        return xa_infer_type_param_from_arg(param_type->map.value_type, arg_type->map.value_type,
                                            tp_name, depth + 1);
    }

    // Task<T> vs Task<int> -> T = int
    if (xr_type_is_named_class(param_type, "Task") && xr_type_is_named_class(arg_type, "Task")) {
        XrType *pt =
            (param_type->instance.type_arg_count > 0) ? param_type->instance.type_args[0] : NULL;
        XrType *at =
            (arg_type->instance.type_arg_count > 0) ? arg_type->instance.type_args[0] : NULL;
        return xa_infer_type_param_from_arg(pt, at, tp_name, depth + 1);
    }

    // Channel<T> vs Channel<int> -> T = int
    if ((param_type->kind == XR_KIND_CHANNEL) && (arg_type->kind == XR_KIND_CHANNEL)) {
        return xa_infer_type_param_from_arg(param_type->container.element_type,
                                            arg_type->container.element_type, tp_name, depth + 1);
    }

    // fn(T): U vs fn(int): string -> T = int, U = string
    if (XR_TYPE_IS_FUNCTION(param_type) && XR_TYPE_IS_FUNCTION(arg_type)) {
        int pc = param_type->function.param_count;
        int ac = arg_type->function.param_count;
        int min = pc < ac ? pc : ac;
        for (int i = 0; i < min; i++) {
            XrType *r =
                xa_infer_type_param_from_arg(param_type->function.param_types[i],
                                             arg_type->function.param_types[i], tp_name, depth + 1);
            if (r)
                return r;
        }
        if (param_type->function.return_type && arg_type->function.return_type) {
            return xa_infer_type_param_from_arg(param_type->function.return_type,
                                                arg_type->function.return_type, tp_name, depth + 1);
        }
    }

    return NULL;
}

// Helper: apply generic type substitution for a call expression
// Builds param_names from symbol links, resolves actual types (explicit or inferred),
// then substitutes into return_type. Returns the substituted type.
XrType *xa_substitute_generic_call(XaInferContext *ctx, XaSymbolLinks *links, XrType *callee_type,
                                   XrType *return_type, CallExprNode *call, int arg_count) {
    XR_DCHECK(ctx != NULL, "substitute_generic_call: NULL ctx");
    XR_DCHECK(links != NULL, "substitute_generic_call: NULL links");
    int type_param_count = xa_symbol_links_get_type_param_count(links);
    if (type_param_count <= 0 || !return_type)
        return return_type;

    const char **param_names = xr_malloc(sizeof(const char *) * type_param_count);
    if (!param_names)
        return return_type;
    for (int i = 0; i < type_param_count; i++) {
        param_names[i] = xa_symbol_links_get_type_param_name(links, i);
    }

    XrType **actual_types = NULL;
    int actual_count = 0;
    bool inferred = false;

    if (call->type_arg_count > 0) {
        actual_types = xr_malloc(sizeof(XrType *) * (size_t) call->type_arg_count);
        if (!actual_types) {
            xr_free(param_names);
            return return_type;
        }
        for (int i = 0; i < call->type_arg_count; i++)
            actual_types[i] = call->type_args[i]
                                  ? xr_tref_resolve(ctx->analyzer->isolate, call->type_args[i])
                                  : xr_type_new_unknown(NULL);
        actual_count = call->type_arg_count;
        inferred = true; /* mark for free */
    } else {
        actual_types = xr_malloc(sizeof(XrType *) * type_param_count);
        if (!actual_types) {
            xr_free(param_names);
            return return_type;
        }
        actual_count = type_param_count;
        inferred = true;

        XrType **param_types = callee_type->function.param_types;
        int param_count = callee_type->function.param_count;

        for (int i = 0; i < type_param_count; i++) {
            actual_types[i] = NULL;
            const char *tp_name = param_names[i];
            for (int j = 0; j < param_count && j < arg_count; j++) {
                XrType *pt = param_types ? param_types[j] : NULL;
                // Use cached type if available (avoid re-evaluating lambdas
                // which would lose callback context and trigger duplicate warnings)
                XrType *cached = xa_analyzer_get_node_type(ctx->analyzer, call->arguments[j]);
                XrType *at = cached ? cached : xa_visit_infer_expr(ctx, call->arguments[j]);
                if (pt && at) {
                    actual_types[i] = xa_infer_type_param_from_arg(pt, at, tp_name, 0);
                    if (actual_types[i])
                        break;
                }
            }
        }
    }

    if (actual_count > 0) {
        return_type = xr_type_substitute(ctx->analyzer->isolate, return_type, param_names,
                                         actual_types, actual_count);
    }

    xr_free(param_names);
    if (inferred && actual_types)
        xr_free(actual_types);
    return return_type;
}

// Check if a function body contains any 'return <expr>' (non-void return).
// Does NOT recurse into nested functions/lambdas.
XR_FUNC bool xa_body_has_return_expr(AstNode *node) {
    if (!node)
        return false;
    switch (node->type) {
        case AST_RETURN_STMT: {
            ReturnStmtNode *ret = &node->as.return_stmt;
            return (ret->value_count > 0 && ret->values && ret->values[0]);
        }
        case AST_BLOCK: {
            BlockNode *block = &node->as.block;
            for (int i = 0; i < block->count; i++) {
                if (xa_body_has_return_expr(block->statements[i]))
                    return true;
            }
            return false;
        }
        case AST_IF_STMT:
            return xa_body_has_return_expr(node->as.if_stmt.then_branch) ||
                   xa_body_has_return_expr(node->as.if_stmt.else_branch);
        case AST_WHILE_STMT:
            return xa_body_has_return_expr(node->as.while_stmt.body);
        case AST_FOR_STMT:
            return xa_body_has_return_expr(node->as.for_stmt.body);
        case AST_FOR_IN_STMT:
            return xa_body_has_return_expr(node->as.for_in_stmt.body);
        case AST_TRY_CATCH:
            return xa_body_has_return_expr(node->as.try_catch.try_body) ||
                   xa_body_has_return_expr(node->as.try_catch.catch_body);
        case AST_MATCH_EXPR: {
            MatchExprNode *m = &node->as.match_expr;
            for (int i = 0; i < m->arm_count; i++) {
                if (m->arms[i] && m->arms[i]->type == AST_MATCH_ARM) {
                    if (xa_body_has_return_expr(m->arms[i]->as.match_arm.body))
                        return true;
                }
            }
            return false;
        }
        // Do NOT recurse into nested functions/lambdas
        case AST_FUNCTION_DECL:
        case AST_FUNCTION_EXPR:
            return false;
        default:
            return false;
    }
}

/* ============================================================================
 * Pass 1: Symbol Collection
 * ============================================================================
 * Collects all symbols (functions, classes, variables) before type inference.
 * Uses two-phase approach for functions/classes to support mutual recursion.
 * ========================================================================== */

// xa_visit_collect_function_decl_only / xa_visit_collect_function_body
// are defined in xanalyzer_visitor_decl.c and declared in
// xanalyzer_visitor_internal.h.

// Helper: collect statements with function hoisting (two-phase).
// Cross-TU: also called from xa_visit_collect_function_body() in
// xanalyzer_visitor_decl.c for nested function bodies.
void xa_visit_collect_statements_with_hoisting(XaInferContext *ctx, AstNode **stmts, int count) {
    // Phase 0: register every interface declaration first so that any class
    // body parsed in Phase 1 can resolve `implements Foo` against a real
    // symbol (Foo could appear before or after the class in source order).
    for (int i = 0; i < count; i++) {
        AstNode *stmt = stmts[i];
        if (!stmt)
            continue;
        if (stmt->type == AST_INTERFACE_DECL) {
            xa_visit_collect_interface(ctx, stmt);
        } else if (stmt->type == AST_EXPORT_STMT && stmt->as.export_stmt.declaration &&
                   stmt->as.export_stmt.declaration->type == AST_INTERFACE_DECL) {
            xa_visit_collect_interface(ctx, stmt->as.export_stmt.declaration);
        }
    }

    // Phase 1: Collect all function/class/enum declarations first (hoisting)
    for (int i = 0; i < count; i++) {
        AstNode *stmt = stmts[i];
        if (!stmt)
            continue;
        if (stmt->type == AST_FUNCTION_DECL) {
            xa_visit_collect_function_decl_only(ctx, stmt);
        } else if (stmt->type == AST_CLASS_DECL || stmt->type == AST_STRUCT_DECL) {
            xa_visit_collect_class(ctx, stmt);
        } else if (stmt->type == AST_ENUM_DECL) {
            xa_visit_collect(ctx, stmt);
        } else if (stmt->type == AST_EXPORT_STMT && stmt->as.export_stmt.declaration) {
            AstNode *decl = stmt->as.export_stmt.declaration;
            if (decl->type == AST_FUNCTION_DECL) {
                xa_visit_collect_function_decl_only(ctx, decl);
            } else if (decl->type == AST_CLASS_DECL || decl->type == AST_STRUCT_DECL) {
                xa_visit_collect_class(ctx, decl);
            } else if (decl->type == AST_ENUM_DECL) {
                xa_visit_collect(ctx, decl);
            }
        }
    }

    // Phase 2: Collect function bodies and other declarations
    for (int i = 0; i < count; i++) {
        AstNode *stmt = stmts[i];
        if (!stmt)
            continue;
        if (stmt->type == AST_FUNCTION_DECL) {
            xa_visit_collect_function_body(ctx, stmt);
        } else if (stmt->type == AST_EXPORT_STMT && stmt->as.export_stmt.declaration) {
            AstNode *decl = stmt->as.export_stmt.declaration;
            if (decl->type == AST_FUNCTION_DECL) {
                xa_visit_collect_function_body(ctx, decl);
            } else if (decl->type != AST_CLASS_DECL && decl->type != AST_STRUCT_DECL &&
                       decl->type != AST_ENUM_DECL) {
                xa_visit_collect(ctx, decl);
            }
        } else if (stmt->type != AST_CLASS_DECL && stmt->type != AST_STRUCT_DECL &&
                   stmt->type != AST_ENUM_DECL) {
            /* Bare block statements need a scope so inner let/const
             * declarations get distinct symbol_ids from outer variables
             * with the same name (variable shadowing).  Matches Pass 2's
             * xa_visit_block_stmt which enters BLOCK_SCOPE. */
            if (stmt->type == AST_BLOCK) {
                xa_analyzer_enter_scope(ctx->analyzer, XA_SCOPE_BLOCK, stmt);
                xa_visit_collect(ctx, stmt);
                xa_analyzer_exit_scope(ctx->analyzer);
            } else {
                xa_visit_collect(ctx, stmt);
            }
        }
    }
}

// Helper: collect import statement (register module variable in symbol table)
static void xa_visit_collect_import(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node)
        return;

    ImportStmtNode *import = &node->as.import_stmt;

    // For whole module import: import math or import math as m
    if (import->member_count == 0) {
        const char *var_name = import->alias ? import->alias : import->module_name;

        XaSymbol *sym = xa_symbol_new(var_name, XA_SYM_MODULE);
        if (sym) {
            sym->location.line = node->line;
            xa_scope_add_symbol(ctx->analyzer->current_scope, sym);
            import->symbol_id = sym->id;

            XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, sym);
            if (links) {
                // Store actual module name for type lookup (handles aliasing)
                links->module_name = import->module_name;
                links->type = xr_type_new_unknown(NULL);
                links->declared_type = links->type;
            }
        }
    } else {
        // For selective import: import { a, b } from "module"
        for (int i = 0; i < import->member_count; i++) {
            ImportMember *member = &import->members[i];
            const char *local_name = member->alias ? member->alias : member->name;

            // Register each imported member as a variable
            XaSymbol *sym = xa_symbol_new(local_name, XA_SYM_IMPORT);
            if (sym) {
                sym->location.line = node->line;
                xa_scope_add_symbol(ctx->analyzer->current_scope, sym);
                member->symbol_id = sym->id;

                XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, sym);
                if (links) {
                    // Try to resolve type from module signatures
                    XrType *member_type = NULL;
                    const char *sig =
                        xa_builtin_get_module_func_signature(import->module_name, member->name);
                    if (sig) {
                        member_type = xa_builtin_parse_full_signature(ctx->analyzer->isolate, sig);
                    }
                    links->type = member_type ? member_type : xr_type_new_unknown(NULL);
                    links->declared_type = links->type;
                }
            }
        }
    }
}

void xa_visit_collect(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node)
        return;

    switch (node->type) {
        case AST_PROGRAM:
            xa_visit_collect_program(ctx, node);
            break;
        case AST_FUNCTION_DECL:
            // When called directly (not from hoisting), do both phases
            xa_visit_collect_function(ctx, node);
            break;
        case AST_CLASS_DECL:
        case AST_STRUCT_DECL:
            xa_visit_collect_class(ctx, node);
            break;
        case AST_VAR_DECL:
        case AST_CONST_DECL:
            xa_visit_collect_var_decl(ctx, node);
            break;
        case AST_IMPORT_STMT:
            xa_visit_collect_import(ctx, node);
            break;
        case AST_ENUM_DECL: {
            EnumDeclNode *edecl = &node->as.enum_decl;
            if (edecl->name) {
                XaSymbol *sym = xa_symbol_new(edecl->name, XA_SYM_ENUM);
                sym->location.line = node->line;
                sym->is_const = true;
                xa_scope_add_symbol(ctx->analyzer->current_scope, sym);
                edecl->symbol_id = sym->id;
                XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, sym);
                links->type = xr_type_new_enum(ctx->analyzer->isolate, edecl->name);
                links->declared_type = links->type;

                // Store enum member names for exhaustiveness checking and
                // infer the backing value type from member initializers.
                // All members must share the same value type (int, float,
                // string, or bool); mixed types are a compile error.
                if (edecl->member_count > 0) {
                    links->enum_member_names =
                        xr_malloc(sizeof(const char *) * edecl->member_count);
                    links->enum_member_count = 0;
                    // Track which literal types appear: bit 0=int, 1=float,
                    // 2=string, 3=bool
                    unsigned seen_types = 0;
                    for (int m = 0; m < edecl->member_count; m++) {
                        AstNode *mem = edecl->members[m];
                        if (mem && mem->type == AST_ENUM_MEMBER && mem->as.enum_member.name) {
                            links->enum_member_names[links->enum_member_count++] =
                                mem->as.enum_member.name;
                            AstNode *val = mem->as.enum_member.value;
                            if (!val) {
                                seen_types |= 1;  // auto-increment → int
                            } else if (val->type == AST_LITERAL_INT) {
                                seen_types |= 1;
                            } else if (val->type == AST_LITERAL_FLOAT) {
                                seen_types |= 2;
                            } else if (val->type == AST_LITERAL_STRING) {
                                seen_types |= 4;
                            } else if (val->type == AST_LITERAL_TRUE ||
                                       val->type == AST_LITERAL_FALSE) {
                                seen_types |= 8;
                            } else {
                                seen_types |= 1;  // unknown literal → treat as int
                            }
                        }
                    }
                    // Default to int when no members have explicit values
                    if (seen_types == 0)
                        seen_types = 1;
                    // Check for mixed types: seen_types must be a power of 2
                    bool mixed = (seen_types & (seen_types - 1)) != 0;
                    if (mixed) {
                        XrLocation loc = {
                            .file = ctx->file_path, .line = node->line, .column = node->column};
                        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                                   XR_ERR_ANALYZE_ENUM_MIXED_TYPE,
                                                   "Enum members must all have the same value type "
                                                   "(int, float, string, or bool)",
                                                   &loc);
                        links->enum_value_type = xr_type_new_int(NULL);
                    } else if (seen_types & 4) {
                        links->enum_value_type = xr_type_new_string(NULL);
                    } else if (seen_types & 2) {
                        links->enum_value_type = xr_type_new_float(NULL);
                    } else if (seen_types & 8) {
                        links->enum_value_type = xr_type_new_bool(NULL);
                    } else {
                        links->enum_value_type = xr_type_new_int(NULL);
                    }
                }
            }
            break;
        }
        case AST_TYPE_ALIAS: {
            TypeAliasNode *ta = &node->as.type_alias;
            if (ta->name) {
                XaSymbol *sym = xa_symbol_new(ta->name, XA_SYM_TYPE_ALIAS);
                sym->location.line = node->line;
                sym->is_const = true;
                // Parser stashes the resolved type in
                // TypeAliasNode::resolved_type. Mirror it into the side
                // table so downstream readers (codegen, LSP) get the
                // same answer through the canonical lookup path.
                XrType *resolved = ta->resolved_type
                                       ? xr_tref_resolve(ctx->analyzer->isolate, ta->resolved_type)
                                       : NULL;
                if (resolved) {
                    xa_analyzer_set_node_type(ctx->analyzer, node, resolved);
                }
                sym->alias_type = resolved;
                xa_scope_add_symbol(ctx->analyzer->current_scope, sym);

                XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, sym);
                if (links) {
                    links->type = resolved ? resolved : xr_type_new_unknown(NULL);
                    links->declared_type = links->type;
                }
            }
            break;
        }
        case AST_FOR_IN_STMT: {
            // Register for-in iteration variable in a block scope
            ForInStmtNode *fi = &node->as.for_in_stmt;
            xa_analyzer_enter_scope(ctx->analyzer, XA_SCOPE_BLOCK, node);
            if (fi->item_name) {
                XaSymbol *sym = xa_symbol_new(fi->item_name, XA_SYM_VARIABLE);
                sym->location.line = node->line;
                sym->is_const = true;  // for-in loop variable is immutable
                xa_scope_add_symbol(ctx->analyzer->current_scope, sym);
                fi->item_symbol_id = sym->id;
                XaSymbolLinks *item_links = xa_analyzer_get_links(ctx->analyzer, sym);
                if (item_links)
                    item_links->is_definitely_assigned = true;
            }
            if (fi->is_keyvalue && fi->value_name) {
                XaSymbol *vsym = xa_symbol_new(fi->value_name, XA_SYM_VARIABLE);
                vsym->location.line = node->line;
                vsym->is_const = true;  // for-in loop variable is immutable
                xa_scope_add_symbol(ctx->analyzer->current_scope, vsym);
                fi->value_symbol_id = vsym->id;
                XaSymbolLinks *val_links = xa_analyzer_get_links(ctx->analyzer, vsym);
                if (val_links)
                    val_links->is_definitely_assigned = true;
            }
            if (fi->body) {
                xa_visit_collect(ctx, fi->body);
            }
            xa_analyzer_exit_scope(ctx->analyzer);
            break;
        }
        case AST_BLOCK:
            /* Bare block statements (standalone { ... }) get a scope from
             * xa_visit_collect_statements_with_hoisting which wraps them.
             * Structural blocks (while/for/if body) call here directly
             * and must NOT get an extra scope — their enclosing construct
             * already manages scoping in Pass 2. */
            xa_visit_collect_statements_with_hoisting(ctx, node->as.block.statements,
                                                      node->as.block.count);
            break;

        // P1-1: Recurse into control flow statements to collect nested declarations
        case AST_FOR_STMT: {
            ForStmtNode *fs = &node->as.for_stmt;
            xa_analyzer_enter_scope(ctx->analyzer, XA_SCOPE_BLOCK, node);
            if (fs->initializer)
                xa_visit_collect(ctx, fs->initializer);
            if (fs->body)
                xa_visit_collect(ctx, fs->body);
            xa_analyzer_exit_scope(ctx->analyzer);
            break;
        }
        case AST_WHILE_STMT:
            if (node->as.while_stmt.body)
                xa_visit_collect(ctx, node->as.while_stmt.body);
            break;
        case AST_IF_STMT:
            if (node->as.if_stmt.then_branch)
                xa_visit_collect(ctx, node->as.if_stmt.then_branch);
            if (node->as.if_stmt.else_branch)
                xa_visit_collect(ctx, node->as.if_stmt.else_branch);
            break;
        case AST_TRY_CATCH: {
            TryCatchNode *tc = &node->as.try_catch;
            if (tc->try_body)
                xa_visit_collect(ctx, tc->try_body);
            if (tc->catch_body) {
                xa_analyzer_enter_scope(ctx->analyzer, XA_SCOPE_BLOCK, node);
                if (tc->catch_var) {
                    XaSymbol *err_sym = xa_symbol_new(tc->catch_var, XA_SYM_VARIABLE);
                    err_sym->location.line = tc->catch_var_line;
                    xa_scope_add_symbol(ctx->analyzer->current_scope, err_sym);
                    XaSymbolLinks *err_links = xa_analyzer_get_links(ctx->analyzer, err_sym);
                    if (err_links) {
                        err_links->type =
                            xr_type_new_named_instance(ctx->analyzer->isolate, "Exception");
                        err_links->is_definitely_assigned = true;
                    }
                }
                xa_visit_collect(ctx, tc->catch_body);
                xa_analyzer_exit_scope(ctx->analyzer);
            }
            if (tc->finally_body)
                xa_visit_collect(ctx, tc->finally_body);
            break;
        }
        case AST_SELECT_STMT: {
            SelectStmtNode *sel = &node->as.select_stmt;
            for (int i = 0; i < sel->case_count; i++) {
                if (sel->cases[i])
                    xa_visit_collect(ctx, sel->cases[i]);
            }
            break;
        }
        case AST_SELECT_CASE: {
            SelectCaseNode *sc = &node->as.select_case;
            /* Recv cases introduce a variable: `msg from ch => ...`
             * Enter a block scope for the case body so `msg` is visible. */
            if (sc->var_name && !sc->is_send && !sc->is_default) {
                xa_analyzer_enter_scope(ctx->analyzer, XA_SCOPE_BLOCK, node);
                XaSymbol *sym = xa_symbol_new(sc->var_name, XA_SYM_VARIABLE);
                sym->location.line = node->line;
                xa_scope_add_symbol(ctx->analyzer->current_scope, sym);
                sc->var_symbol_id = sym->id;
                XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, sym);
                if (links)
                    links->is_definitely_assigned = true;
                if (sc->body)
                    xa_visit_collect(ctx, sc->body);
                xa_analyzer_exit_scope(ctx->analyzer);
            } else {
                if (sc->body)
                    xa_visit_collect(ctx, sc->body);
            }
            break;
        }
        case AST_SCOPE_BLOCK:
            /* Pass 2's xa_visit_block_stmt enters a BLOCK scope keyed on the
             * body AST node. Pass 1 must create the matching scope so that
             * nested constructs (e.g. for-in) register their symbols under
             * the same parent chain. Otherwise the for-in scope from Pass 1
             * becomes a sibling of Pass 2's lookup root and its loop
             * variable lookup fails. */
            if (node->as.scope_block.body) {
                AstNode *body = node->as.scope_block.body;
                if (body->type == AST_BLOCK) {
                    xa_analyzer_enter_scope(ctx->analyzer, XA_SCOPE_BLOCK, body);
                    xa_visit_collect(ctx, body);
                    xa_analyzer_exit_scope(ctx->analyzer);
                } else {
                    xa_visit_collect(ctx, body);
                }
            }
            break;
        case AST_EXPR_STMT:
            // Recurse into expression statements (may contain function expressions etc.)
            break;

        // P1-2: Collect destructuring and multi-var declarations
        case AST_DESTRUCTURE_DECL: {
            DestructureDeclNode *dd = &node->as.destructure_decl;
            if (dd->pattern) {
                XrDestructurePattern *pat = dd->pattern;
                if (pat->type == PATTERN_ARRAY || pat->type == PATTERN_TUPLE) {
                    for (int i = 0; i < pat->as.array.element_count; i++) {
                        XrDestructurePattern *elem = pat->as.array.elements[i];
                        if (elem && elem->type == PATTERN_IDENTIFIER && elem->as.identifier.name) {
                            XaSymbol *sym =
                                xa_symbol_new(elem->as.identifier.name, XA_SYM_VARIABLE);
                            sym->is_const = dd->is_const;
                            sym->location.line = node->line;
                            xa_scope_add_symbol(ctx->analyzer->current_scope, sym);
                            elem->as.identifier.symbol_id = sym->id;
                        }
                    }
                } else if (pat->type == PATTERN_OBJECT) {
                    for (int i = 0; i < pat->as.object.field_count; i++) {
                        XrDestructurePattern *vp = pat->as.object.patterns[i];
                        if (vp && vp->type == PATTERN_IDENTIFIER && vp->as.identifier.name) {
                            XaSymbol *sym = xa_symbol_new(vp->as.identifier.name, XA_SYM_VARIABLE);
                            sym->is_const = dd->is_const;
                            sym->location.line = node->line;
                            xa_scope_add_symbol(ctx->analyzer->current_scope, sym);
                            vp->as.identifier.symbol_id = sym->id;
                        }
                    }
                }
            }
            break;
        }
        default:
            break;
    }
}

void xa_visit_collect_program(XaInferContext *ctx, AstNode *node) {
    if (!node || node->type != AST_PROGRAM)
        return;

    ProgramNode *prog = &node->as.program;
    // Use hoisting to support mutual recursion at module level
    xa_visit_collect_statements_with_hoisting(ctx, prog->statements, prog->count);
}

/* ============================================================================
 * Pass 2: Type Inference - Entry Points
 * ============================================================================
 * xa_visit_infer()      - Main dispatch for expressions and statements
 * xa_visit_infer_expr() - Expression type inference
 * xa_visit_infer_stmt() - Statement type inference
 * ========================================================================== */

XrType *xa_visit_infer(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node)
        return xr_type_new_unknown(NULL);

    // Check if it's an expression or statement
    switch (node->type) {
        // Expressions
        case AST_LITERAL_INT:
        case AST_LITERAL_FLOAT:
        case AST_LITERAL_STRING:
        case AST_LITERAL_BIGINT:
        case AST_LITERAL_REGEX:
        case AST_LITERAL_NULL:
        case AST_LITERAL_TRUE:
        case AST_LITERAL_FALSE:
        case AST_VARIABLE:
        case AST_BINARY_ADD:
        case AST_BINARY_SUB:
        case AST_BINARY_MUL:
        case AST_BINARY_DIV:
        case AST_CALL_EXPR:
        case AST_MEMBER_ACCESS:
        case AST_INDEX_GET:
        case AST_ARRAY_LITERAL:
        case AST_TUPLE_LITERAL:
        case AST_MAP_LITERAL:
        case AST_OBJECT_LITERAL:
        case AST_NEW_EXPR:
        case AST_TERNARY:
        case AST_FUNCTION_EXPR:
            return xa_visit_infer_expr(ctx, node);

        // Statements
        default:
            xa_visit_infer_stmt(ctx, node);
            return xr_type_new_unit(NULL);
    }
}

XrType *xa_visit_infer_expr(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node)
        return xr_type_new_unknown(NULL);

    XrType *result;

    switch (node->type) {
        case AST_LITERAL_INT:
            result = xr_type_new_int(NULL);
            break;
        case AST_LITERAL_FLOAT:
            result = xr_type_new_float(NULL);
            break;
        case AST_LITERAL_STRING:
            result = xr_type_new_string(NULL);
            break;
        case AST_TEMPLATE_STRING:
            /* Visit all interpolation parts to resolve variable symbol_ids. */
            for (int ti = 0; ti < node->as.template_str.part_count; ti++) {
                if (node->as.template_str.parts[ti])
                    xa_visit_infer_expr(ctx, node->as.template_str.parts[ti]);
            }
            result = xr_type_new_string(NULL);
            break;
        case AST_LITERAL_BIGINT:
            result = xr_type_new_bigint(ctx->analyzer->isolate);
            break;
        case AST_LITERAL_REGEX:
            result = xr_type_new_regex(ctx->analyzer->isolate);
            break;
        case AST_LITERAL_NULL:
            result = xr_type_new_null(NULL);
            break;
        case AST_LITERAL_TRUE:
        case AST_LITERAL_FALSE:
            result = xr_type_new_bool(NULL);
            break;
        case AST_VARIABLE:
            result = xa_visit_variable(ctx, node);
            break;
        case AST_BINARY_ADD:
        case AST_BINARY_SUB:
        case AST_BINARY_MUL:
        case AST_BINARY_DIV:
        case AST_BINARY_MOD:
        case AST_BINARY_EQ:
        case AST_BINARY_NE:
        case AST_BINARY_LT:
        case AST_BINARY_LE:
        case AST_BINARY_GT:
        case AST_BINARY_GE:
        case AST_BINARY_AND:
        case AST_BINARY_OR:
        case AST_BINARY_EQ_STRICT:
        case AST_BINARY_NE_STRICT:
        case AST_BINARY_BAND:
        case AST_BINARY_BOR:
        case AST_BINARY_BXOR:
        case AST_BINARY_LSHIFT:
        case AST_BINARY_RSHIFT:
            result = xa_visit_binary(ctx, node);
            break;
        case AST_UNARY_NEG:
        case AST_UNARY_NOT:
        case AST_UNARY_BNOT:
            result = xa_visit_unary(ctx, node);
            break;
        case AST_CALL_EXPR:
            result = xa_visit_call(ctx, node);
            break;
        case AST_MEMBER_ACCESS:
            result = xa_visit_member_access(ctx, node);
            break;
        case AST_INDEX_GET:
            result = xa_visit_index_get(ctx, node);
            break;
        case AST_ARRAY_LITERAL:
            result = xa_visit_array_literal(ctx, node);
            break;
        case AST_TUPLE_LITERAL:
            result = xa_visit_tuple_literal(ctx, node);
            break;
        case AST_MAP_LITERAL:
            result = xa_visit_map_literal(ctx, node);
            break;
        case AST_OBJECT_LITERAL:
            result = xa_visit_object_literal(ctx, node);
            break;
        case AST_NEW_EXPR:
            result = xa_visit_new_expr(ctx, node);
            break;
        case AST_STRUCT_LITERAL:
            result = xa_visit_struct_literal(ctx, node);
            break;
        case AST_TERNARY:
            result = xa_visit_ternary(ctx, node);
            break;
        case AST_FUNCTION_EXPR:
            result = xa_visit_function_expr(ctx, node);
            break;
        case AST_GROUPING:
            result = xa_visit_infer_expr(ctx, node->as.grouping);
            break;
        case AST_GO_EXPR:
            result = xa_visit_go_expr(ctx, node);
            break;
        case AST_AWAIT_EXPR:
            result = xa_visit_await_expr(ctx, node);
            break;
        case AST_MATCH_EXPR:
            result = xa_visit_match_expr(ctx, node);
            break;
        case AST_NULLISH_COALESCE:
            result = xa_visit_nullish_coalesce(ctx, node);
            break;
        case AST_OPTIONAL_CHAIN:
            result = xa_visit_optional_chain(ctx, node);
            break;
        case AST_FORCE_UNWRAP:
            result = xa_visit_force_unwrap(ctx, node);
            break;
        case AST_AS_EXPR:
            result = xa_visit_as_expr(ctx, node);
            break;
        case AST_IS_EXPR:
            if (node->as.is_expr.expr)
                xa_visit_infer_expr(ctx, node->as.is_expr.expr);
            result = xr_type_new_bool(NULL);
            break;
        case AST_RANGE:
            if (node->as.range.start)
                xa_visit_infer_expr(ctx, node->as.range.start);
            if (node->as.range.end)
                xa_visit_infer_expr(ctx, node->as.range.end);
            result = xr_type_new_named_instance(ctx->analyzer->isolate, "Range");
            break;
        case AST_SET_LITERAL: {
            XrType *elem = NULL;
            if (ctx->expected_type && ctx->expected_type->kind == XR_KIND_SET &&
                ctx->expected_type->container.element_type) {
                elem = ctx->expected_type->container.element_type;
            }
            /* Visit ALL elements to resolve symbol_ids; infer elem type from first. */
            for (int si = 0; si < node->as.set_literal.count; si++) {
                if (node->as.set_literal.elements[si]) {
                    XrType *et = xa_visit_infer_expr(ctx, node->as.set_literal.elements[si]);
                    if (!elem && si == 0)
                        elem = et;
                }
            }
            if (!elem)
                elem = xr_type_new_unknown(NULL);
            result = xr_type_new_set(ctx->analyzer->isolate, elem);
            break;
        }
        case AST_CHANNEL_NEW:
            // Visit buffer size expression to resolve variable symbol_ids
            if (node->as.channel_new.buffer_size)
                xa_visit_infer_expr(ctx, node->as.channel_new.buffer_size);
            // Use expected_type if available (from type annotation: Channel<T>)
            if (ctx->expected_type && (ctx->expected_type->kind == XR_KIND_CHANNEL)) {
                result = ctx->expected_type;
            } else {
                result = xr_type_new_channel(ctx->analyzer->isolate, xr_type_new_unknown(NULL));
            }
            break;
        case AST_MOVE_EXPR:
            result = xa_visit_move_expr(ctx, node);
            break;
        case AST_SUPER_CALL: {
            /* Visit all arguments to resolve symbol_ids. */
            for (int si = 0; si < node->as.super_call.arg_count; si++) {
                if (node->as.super_call.arguments[si])
                    xa_visit_infer_expr(ctx, node->as.super_call.arguments[si]);
            }
            /* Resolve super.method() return type by looking up the method
             * in the base class chain via class_info. */
            XaScope *s = ctx->analyzer->current_scope;
            while (s && s->kind != XA_SCOPE_CLASS)
                s = s->parent;
            if (s && s->class_symbol) {
                XaSymbolLinks *cl = xa_analyzer_get_links(ctx->analyzer, s->class_symbol);
                if (cl && cl->class_info && cl->class_info->base) {
                    const char *mname = node->as.super_call.method_name;
                    if (mname) {
                        XaSymbol *member = xa_class_info_lookup_member(cl->class_info->base, mname);
                        if (member) {
                            XaSymbolLinks *ml = xa_analyzer_get_links(ctx->analyzer, member);
                            if (ml && ml->type) {
                                /* Method type is a function type; extract
                                 * return type for the call result. */
                                if (XR_TYPE_IS_FUNCTION(ml->type) &&
                                    ml->type->function.return_type) {
                                    result = ml->type->function.return_type;
                                } else {
                                    result = ml->type;
                                }
                                break;
                            }
                        }
                    }
                }
            }
            result = xr_type_new_unknown(NULL);
            break;
        }
        case AST_THIS_EXPR: {
            /* Walk up scopes to find the enclosing class scope and
             * return the class instance type so this.field resolves. */
            XaScope *s = ctx->analyzer->current_scope;
            while (s && s->kind != XA_SCOPE_CLASS)
                s = s->parent;
            if (s && s->class_symbol) {
                const char *cname = s->class_symbol->name;
                if (cname) {
                    XaSymbolLinks *cl = xa_analyzer_get_links(ctx->analyzer, s->class_symbol);
                    bool is_struct = cl && cl->type && cl->type->is_value_type;
                    result = xr_type_new_named_instance(ctx->analyzer->isolate, cname);
                    if (result)
                        result->is_value_type = is_struct;
                } else {
                    result = xr_type_new_unknown(NULL);
                }
            } else {
                result = xr_type_new_unknown(NULL);
            }
            break;
        }
        case AST_SLICE_EXPR: {
            SliceExprNode *sl = &node->as.slice_expr;
            XrType *src = sl->source ? xa_visit_infer_expr(ctx, sl->source) : NULL;
            if (sl->start)
                xa_visit_infer_expr(ctx, sl->start);
            if (sl->end)
                xa_visit_infer_expr(ctx, sl->end);
            /* Slice of array/string returns same container type. */
            if (src && XR_TYPE_IS_ARRAY(src)) {
                result = src;
            } else if (src && XR_TYPE_IS_STRING(src)) {
                result = xr_type_new_string(NULL);
            } else {
                result = xr_type_new_unknown(NULL);
            }
            break;
        }
        case AST_ENUM_CONVERT: {
            if (node->as.enum_convert.value_expr)
                xa_visit_infer_expr(ctx, node->as.enum_convert.value_expr);
            result = node->as.enum_convert.enum_name
                         ? xr_type_new_enum(ctx->analyzer->isolate, node->as.enum_convert.enum_name)
                         : xr_type_new_unknown(NULL);
            break;
        }
        case AST_ENUM_ACCESS:
            result = node->as.enum_access.enum_name
                         ? xr_type_new_enum(ctx->analyzer->isolate, node->as.enum_access.enum_name)
                         : xr_type_new_unknown(NULL);
            break;
        case AST_CANCELLED_EXPR:
            result = xr_type_new_bool(NULL);
            break;
        default:
            result = xr_type_new_unknown(NULL);
            break;
    }

    // Propagate is_value_type for class/instance types created from type annotations
    // (parser doesn't know if a type name is struct vs class, so is_value_type defaults to false)
    if (result && !result->is_value_type &&
        (result->kind == XR_KIND_CLASS || result->kind == XR_KIND_INSTANCE) &&
        result->instance.class_name) {
        XaSymbol *_vs = xa_scope_lookup(ctx->analyzer->global_scope, result->instance.class_name);
        if (_vs && _vs->kind == XA_SYM_CLASS) {
            XaSymbolLinks *_vl = xa_analyzer_get_links(ctx->analyzer, _vs);
            if (_vl && _vl->type && _vl->type->is_value_type) {
                result->is_value_type = true;
            }
        }
    }

    // Track unknown type count for inference quality metric
    if (result && XR_TYPE_IS_UNKNOWN(result)) {
        ctx->analyzer->unknown_type_count++;
    }

    // Cache inferred type in the analyzer side table for codegen.
    xa_analyzer_set_node_type(ctx->analyzer, node, result);
    return result;
}

/*
 * Unified function body visitor.
 * Handles both named functions (Pass 1 already collected) and
 * function expressions (Pass 1 never reached).
 *
 * Always call xa_visit_collect() on body — it is idempotent.
 * For Pass 1 scopes: symbols already exist, hashmap_set overwrites harmlessly.
 * For new scopes: symbols are registered fresh.
 * Body is always visited directly (no xa_visit_block_stmt) to ensure
 * consistent FUNCTION_SCOPE structure.
 */
void xa_visit_function_body_unified(XaInferContext *ctx, AstNode *body) {
    if (!body)
        return;

    // Collect body declarations (idempotent: safe on both new and reused scopes)
    xa_visit_collect(ctx, body);

    // Visit body statements directly (skip xa_visit_block_stmt)
    if (body->type == AST_BLOCK) {
        BlockNode *blk = &body->as.block;
        for (int si = 0; si < blk->count; si++) {
            xa_visit_infer_stmt(ctx, blk->statements[si]);
        }
    } else {
        xa_visit_infer_stmt(ctx, body);
    }
}

void xa_visit_infer_stmt(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node)
        return;

    switch (node->type) {
        case AST_PROGRAM:
            for (int i = 0; i < node->as.program.count; i++) {
                xa_visit_infer_stmt(ctx, node->as.program.statements[i]);
            }
            break;
        case AST_BLOCK:
            xa_visit_block_stmt(ctx, node);
            break;
        case AST_VAR_DECL:
        case AST_CONST_DECL:
            xa_visit_var_decl_stmt(ctx, node);
            break;
        case AST_ASSIGNMENT:
            xa_visit_assignment_stmt(ctx, node);
            break;
        case AST_INC:
        case AST_DEC: {
            IncDecNode *id = &node->as.inc;
            XaSymbol *id_sym = xa_scope_lookup(ctx->analyzer->current_scope, id->name);
            if (id_sym)
                id->symbol_id = id_sym->id;
            if (id_sym && id_sym->is_const) {
                XrLocation loc = {
                    .file = ctx->file_path, .line = node->line, .column = node->column};
                char msg[128];
                snprintf(msg, sizeof(msg), "Cannot modify const '%s'", id->name);
                xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                           XR_ERR_ANALYZE_CONST_ASSIGN, msg, &loc);
            }
            break;
        }
        case AST_COMPOUND_ASSIGNMENT: {
            // Infer value expression so its type is recorded in the side table
            CompoundAssignmentNode *ca = &node->as.compound_assignment;
            if (ca->value)
                xa_visit_infer_expr(ctx, ca->value);
            // Visit object expression for member compound assign (obj.field += ...)
            if (ca->object)
                xa_visit_infer_expr(ctx, ca->object);
            // Check const/in-param immutability
            XaSymbol *ca_sym = xa_scope_lookup(ctx->analyzer->current_scope, ca->name);
            if (ca_sym)
                ca->symbol_id = ca_sym->id;
            if (ca_sym && ca_sym->is_const) {
                XrLocation loc = {
                    .file = ctx->file_path, .line = node->line, .column = node->column};
                char msg[128];
                snprintf(msg, sizeof(msg), "Cannot assign to const '%s'", ca->name);
                xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                           XR_ERR_ANALYZE_CONST_ASSIGN, msg, &loc);
            }
            break;
        }
        case AST_MEMBER_SET: {
            // Infer types for member set expression
            MemberSetNode *ms = &node->as.member_set;
            XrType *obj_type = xa_visit_infer_expr(ctx, ms->object);

            // Bidirectional inference: propagate field declared type to value
            XrType *saved_expected = ctx->expected_type;
            if (ms->member) {
                XaSymbol *class_sym = NULL;
                // Case 1: this.field = expr — find class from scope chain
                if (ms->object && ms->object->type == AST_THIS_EXPR) {
                    XaScope *s = ctx->analyzer->current_scope;
                    while (s) {
                        if (s->class_symbol) {
                            class_sym = s->class_symbol;
                            break;
                        }
                        s = s->parent;
                    }
                }
                // Case 2: obj.field = expr — find class from instance type
                else if (XR_TYPE_IS_INSTANCE(obj_type) && obj_type->instance.class_name) {
                    class_sym =
                        xa_scope_lookup(ctx->analyzer->global_scope, obj_type->instance.class_name);
                }
                if (class_sym) {
                    XaSymbolLinks *class_links = xa_analyzer_get_links(ctx->analyzer, class_sym);
                    if (class_links && class_links->class_info) {
                        XaSymbol *field =
                            xa_class_info_lookup_member(class_links->class_info, ms->member);
                        if (field) {
                            XaSymbolLinks *fl = xa_analyzer_get_links(ctx->analyzer, field);
                            if (fl && fl->type && !XR_TYPE_IS_UNKNOWN(fl->type)) {
                                ctx->expected_type = fl->type;
                            }
                        }
                    }
                }
            }
            xa_visit_infer_expr(ctx, ms->value);
            ctx->expected_type = saved_expected;
            if (XR_TYPE_IS_JSON(obj_type) && obj_type->object.field_count > 0 && ms->member) {
                int field_idx = json_field_index_local(obj_type, ms->member);
                XrLocation loc = {
                    .file = ctx->file_path, .line = node->line, .column = node->column};
                if (field_idx >= 0) {
                    bool readonly = obj_type->object.field_readonly
                                        ? obj_type->object.field_readonly[field_idx]
                                        : false;
                    if (readonly) {
                        char msg[256];
                        snprintf(msg, sizeof(msg), "无法修改只读字段 '%s.%s'（const 修饰）",
                                 json_type_label_local(obj_type), ms->member);
                        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                                   XR_ERR_ANALYZE_CONST_ASSIGN, msg, &loc);
                    }
                } else if (obj_type->object.is_sealed) {
                    char msg[256];
                    snprintf(msg, sizeof(msg), "类型 '%s' 不允许添加字段 '%s'",
                             json_type_label_local(obj_type), ms->member);
                    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                               XR_ERR_ANALYZE_TYPE_MISMATCH, msg, &loc);
                }
            }
            // Check in-parameter immutability: v.x = ... where v is 'in' param
            if (ms->object && ms->object->type == AST_VARIABLE) {
                const char *obj_name = ms->object->as.variable.name;
                XaSymbol *obj_sym = xa_scope_lookup(ctx->analyzer->current_scope, obj_name);
                if (obj_sym && obj_sym->kind == XA_SYM_PARAMETER &&
                    obj_sym->passing_mode == XR_PARAM_IN) {
                    XrLocation loc = {
                        .file = ctx->file_path, .line = node->line, .column = node->column};
                    char msg[256];
                    snprintf(msg, sizeof(msg),
                             "Cannot modify field '%s' of 'in' parameter '%s' (readonly reference)",
                             ms->member, obj_name);
                    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                               XR_ERR_ANALYZE_CONST_ASSIGN, msg, &loc);
                }
            }
            break;
        }
        case AST_IF_STMT:
            xa_visit_if_stmt(ctx, node);
            break;
        case AST_WHILE_STMT:
            xa_visit_while_stmt(ctx, node);
            break;
        case AST_FOR_STMT:
            xa_visit_for_stmt(ctx, node);
            break;
        case AST_RETURN_STMT:
            xa_visit_return_stmt(ctx, node);
            break;
        case AST_EXPR_STMT: {
            AstNode *inner = node->as.expr_stmt;
            // Check in-parameter immutability before normal inference
            if (inner && inner->type == AST_ASSIGNMENT) {
                AssignmentNode *a = &inner->as.assignment;
                XaSymbol *s = xa_scope_lookup(ctx->analyzer->current_scope, a->name);
                if (s && s->kind == XA_SYM_PARAMETER && s->passing_mode == XR_PARAM_IN) {
                    XrLocation loc = {
                        .file = ctx->file_path, .line = inner->line, .column = inner->column};
                    char msg[128];
                    snprintf(msg, sizeof(msg),
                             "Cannot assign to 'in' parameter '%s' (readonly reference)", a->name);
                    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                               XR_ERR_ANALYZE_CONST_ASSIGN, msg, &loc);
                }
            } else if (inner && inner->type == AST_MEMBER_SET) {
                MemberSetNode *ms = &inner->as.member_set;
                if (ms->object && ms->object->type == AST_VARIABLE) {
                    const char *obj_name = ms->object->as.variable.name;
                    XaSymbol *s = xa_scope_lookup(ctx->analyzer->current_scope, obj_name);
                    if (s && s->kind == XA_SYM_PARAMETER && s->passing_mode == XR_PARAM_IN) {
                        XrLocation loc = {
                            .file = ctx->file_path, .line = inner->line, .column = inner->column};
                        char msg[256];
                        snprintf(
                            msg, sizeof(msg),
                            "Cannot modify field '%s' of 'in' parameter '%s' (readonly reference)",
                            ms->member, obj_name);
                        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                                   XR_ERR_ANALYZE_CONST_ASSIGN, msg, &loc);
                    }
                }
            }
            if (inner && (inner->type == AST_MEMBER_SET || inner->type == AST_ASSIGNMENT ||
                          inner->type == AST_COMPOUND_ASSIGNMENT || inner->type == AST_INC ||
                          inner->type == AST_DEC || inner->type == AST_INDEX_SET)) {
                xa_visit_infer_stmt(ctx, inner);
            } else {
                xa_visit_infer_expr(ctx, inner);
            }
            break;
        }
        case AST_FUNCTION_DECL: {
            // Already collected in pass 1, infer body
            FunctionDeclNode *fn_decl = &node->as.function_decl;

            // Save/restore return type state for nested functions
            XrType **saved_return_types = ctx->return_types;
            int saved_return_count = ctx->return_type_count;
            int saved_return_cap = ctx->return_type_capacity;
            ctx->return_types = NULL;
            ctx->return_type_count = 0;
            ctx->return_type_capacity = 0;

            // Isolate flow graph: each function gets a fresh start node
            // so that flow facts from sibling/parent functions (e.g.
            // `let name = p?.name`) do not leak into this function body.
            XaFlowNode *saved_flow = NULL;
            XrFlowLabel *saved_break = NULL;
            XrFlowLabel *saved_continue = NULL;
            XrFlowLabel *saved_return = NULL;
            XrFlowLabel *saved_exception = NULL;
            if (ctx->flow) {
                saved_flow = ctx->flow->current_flow;
                saved_break = ctx->flow->current_break_target;
                saved_continue = ctx->flow->current_continue_target;
                saved_return = ctx->flow->current_return_target;
                saved_exception = ctx->flow->current_exception_target;
                xa_flow_create_start(ctx->flow);
                ctx->flow->current_break_target = NULL;
                ctx->flow->current_continue_target = NULL;
                ctx->flow->current_return_target = NULL;
                ctx->flow->current_exception_target = NULL;
            }

            xa_analyzer_enter_scope(ctx->analyzer, XA_SCOPE_FUNCTION, node);

            // Call-site inference feedback: update unannotated parameter types
            // with types inferred from call-sites during Pass 2.
            if (fn_decl->name) {
                XaSymbol *fn_sym =
                    xa_scope_lookup(ctx->analyzer->current_scope->parent, fn_decl->name);
                XaSymbolLinks *fn_links =
                    fn_sym ? xa_analyzer_get_links(ctx->analyzer, fn_sym) : NULL;
                if (fn_links && fn_links->inferred_param_types) {
                    for (int pi = 0;
                         pi < fn_decl->param_count && pi < fn_links->inferred_param_count; pi++) {
                        XrType *inferred = fn_links->inferred_param_types[pi];
                        if (!inferred || XR_TYPE_IS_UNKNOWN(inferred))
                            continue;
                        XrParamNode *p = fn_decl->params[pi];
                        if (!p || !p->name || p->type)
                            continue;  // skip annotated params
                        XaSymbol *param_sym =
                            xa_scope_lookup_local(ctx->analyzer->current_scope, p->name);
                        if (param_sym) {
                            XaSymbolLinks *pl = xa_analyzer_get_links(ctx->analyzer, param_sym);
                            if (pl && (!pl->type || XR_TYPE_IS_UNKNOWN(pl->type))) {
                                pl->type = inferred;
                            }
                        }
                    }
                }
            }

            // Set expected_return_type for return type checking.
            // Named functions: explicit annotation → use it; omitted → void.
            // This ensures xa_visit_return_stmt catches type mismatches
            // (e.g. return true in :int fn, or return 42 in :void fn).
            XrType *saved_expected_ret = ctx->expected_return_type;
            if (fn_decl->return_type) {
                ctx->expected_return_type =
                    xr_tref_resolve(ctx->analyzer->isolate, fn_decl->return_type);
            } else {
                ctx->expected_return_type = xr_type_new_unit(NULL);
            }

            // Unified body visitor: idempotent collect + direct traversal
            xa_visit_function_body_unified(ctx, fn_decl->body);

            // Re-check inferred_param_types after body analysis: recursive calls
            // inside the body may have widened param types (e.g. Json → Json?).
            if (fn_decl->name) {
                XaSymbol *fn_sym2 =
                    xa_scope_lookup(ctx->analyzer->current_scope->parent, fn_decl->name);
                XaSymbolLinks *fn_links2 =
                    fn_sym2 ? xa_analyzer_get_links(ctx->analyzer, fn_sym2) : NULL;
                if (fn_links2 && fn_links2->inferred_param_types) {
                    for (int pi = 0;
                         pi < fn_decl->param_count && pi < fn_links2->inferred_param_count; pi++) {
                        XrType *inferred = fn_links2->inferred_param_types[pi];
                        if (!inferred || XR_TYPE_IS_UNKNOWN(inferred))
                            continue;
                        XrParamNode *p = fn_decl->params[pi];
                        if (!p || !p->name || p->type)
                            continue;
                        XaSymbol *param_sym =
                            xa_scope_lookup_local(ctx->analyzer->current_scope, p->name);
                        if (param_sym) {
                            XaSymbolLinks *pl = xa_analyzer_get_links(ctx->analyzer, param_sym);
                            if (pl && pl->type && !xr_type_equals(pl->type, inferred)) {
                                pl->type = inferred;
                            }
                        }
                    }
                }
            }

            // Infer return type from collected returns (while still in scope)
            if (!fn_decl->return_type && fn_decl->name) {
                XrType *inferred_ret = xa_infer_compute_return_type(ctx);
                if (inferred_ret && !XR_TYPE_IS_UNKNOWN(inferred_ret) &&
                    !XR_TYPE_IS_UNIT(inferred_ret)) {
                    XaSymbol *fn_sym =
                        xa_scope_lookup(ctx->analyzer->current_scope->parent, fn_decl->name);
                    if (fn_sym && fn_sym->kind == XA_SYM_FUNCTION) {
                        XaSymbolLinks *fn_links = xa_analyzer_get_links(ctx->analyzer, fn_sym);
                        if (fn_links && fn_links->type && XR_TYPE_IS_FUNCTION(fn_links->type)) {
                            fn_links->type->function.return_type = inferred_ret;
                            fn_links->return_type = inferred_ret;
                        }
                    }
                }
            }

            ctx->expected_return_type = saved_expected_ret;

            xa_analyzer_exit_scope(ctx->analyzer);

            // Restore flow state to outer function's context
            if (ctx->flow) {
                ctx->flow->current_flow = saved_flow;
                ctx->flow->current_break_target = saved_break;
                ctx->flow->current_continue_target = saved_continue;
                ctx->flow->current_return_target = saved_return;
                ctx->flow->current_exception_target = saved_exception;
            }

            // Restore outer function's return type state
            if (ctx->return_types)
                xr_free(ctx->return_types);
            ctx->return_types = saved_return_types;
            ctx->return_type_count = saved_return_count;
            ctx->return_type_capacity = saved_return_cap;
            break;
        }
        case AST_EXPORT_STMT:
            // Recurse into the exported declaration
            if (node->as.export_stmt.declaration) {
                xa_visit_infer_stmt(ctx, node->as.export_stmt.declaration);
            }
            break;
        case AST_CLASS_DECL:
        case AST_STRUCT_DECL: {
            // Infer method bodies inside the class/struct
            ClassDeclNode *cls =
                (node->type == AST_STRUCT_DECL) ? &node->as.struct_decl : &node->as.class_decl;
            xa_analyzer_enter_scope(ctx->analyzer, XA_SCOPE_CLASS, node);
            for (int i = 0; i < cls->method_count; i++) {
                if (cls->methods[i] && cls->methods[i]->as.method_decl.body) {
                    xa_analyzer_enter_scope(ctx->analyzer, XA_SCOPE_FUNCTION, cls->methods[i]);
                    // Save/restore expected_return_type so the enclosing
                    // function's type doesn't leak into method bodies.
                    XrType *saved_ret = ctx->expected_return_type;
                    MethodDeclNode *md = &cls->methods[i]->as.method_decl;
                    if (md->return_type) {
                        ctx->expected_return_type =
                            xr_tref_resolve(ctx->analyzer->isolate, md->return_type);
                    } else {
                        ctx->expected_return_type = NULL;
                    }
                    xa_visit_function_body_unified(ctx, md->body);
                    ctx->expected_return_type = saved_ret;
                    xa_analyzer_exit_scope(ctx->analyzer);
                }
            }
            xa_analyzer_exit_scope(ctx->analyzer);
            break;
        }
        case AST_TRY_CATCH: {
            TryCatchNode *tc = &node->as.try_catch;
            if (tc->try_body)
                xa_visit_infer_stmt(ctx, tc->try_body);
            if (tc->catch_body) {
                xa_analyzer_enter_scope(ctx->analyzer, XA_SCOPE_BLOCK, node);
                // Register catch variable as Exception type
                if (tc->catch_var) {
                    XaSymbol *err_sym = xa_symbol_new(tc->catch_var, XA_SYM_VARIABLE);
                    err_sym->location.line = tc->catch_var_line;
                    err_sym->location.column = tc->catch_var_column;
                    xa_scope_add_symbol(ctx->analyzer->current_scope, err_sym);
                    XaSymbolLinks *err_links = xa_analyzer_get_links(ctx->analyzer, err_sym);
                    if (err_links) {
                        err_links->type =
                            xr_type_new_named_instance(ctx->analyzer->isolate, "Exception");
                        err_links->is_definitely_assigned = true;
                    }
                }
                xa_visit_infer_stmt(ctx, tc->catch_body);
                xa_analyzer_exit_scope(ctx->analyzer);
            }
            if (tc->finally_body)
                xa_visit_infer_stmt(ctx, tc->finally_body);
            break;
        }
        case AST_THROW_STMT:
            if (node->as.throw_stmt.expression) {
                xa_visit_infer_expr(ctx, node->as.throw_stmt.expression);
            }
            // Mark flow as unreachable after throw
            if (ctx->flow) {
                ctx->flow->current_flow = ctx->flow->unreachable_flow;
            }
            break;
        case AST_TYPE_ALIAS:
            // Type alias is compile-time concept, already registered in pass 1
            break;
        case AST_FOR_IN_STMT: {
            ForInStmtNode *fi = &node->as.for_in_stmt;

            // Enter the scope created in pass 1
            xa_analyzer_enter_scope(ctx->analyzer, XA_SCOPE_BLOCK, node);

            // Infer collection type
            XrType *coll_type = NULL;
            bool is_enum_iter = false;
            const char *enum_name = NULL;

            if (fi->collection) {
                if (fi->collection->type == AST_RANGE) {
                    // Visit range sub-expressions so variables get symbol_id resolved
                    if (fi->collection->as.range.start)
                        xa_visit_infer_expr(ctx, fi->collection->as.range.start);
                    if (fi->collection->as.range.end)
                        xa_visit_infer_expr(ctx, fi->collection->as.range.end);
                    coll_type = xr_type_new_int(NULL);
                } else if (fi->collection->type == AST_VARIABLE) {
                    // Check if variable is an enum
                    XaSymbol *coll_sym = xa_scope_lookup(ctx->analyzer->current_scope,
                                                         fi->collection->as.variable.name);
                    if (coll_sym && coll_sym->kind == XA_SYM_ENUM) {
                        is_enum_iter = true;
                        // Visit the collection variable so its symbol_id is resolved
                        xa_visit_infer_expr(ctx, fi->collection);
                        XaSymbolLinks *coll_links = xa_analyzer_get_links(ctx->analyzer, coll_sym);
                        if (coll_links && coll_links->type) {
                            enum_name = coll_links->type->enum_type.enum_name;
                        }
                    } else {
                        coll_type = xa_visit_infer_expr(ctx, fi->collection);
                    }
                } else {
                    coll_type = xa_visit_infer_expr(ctx, fi->collection);
                }
            }

            /*
             * Unified for-in type inference rules:
             *   Array<T>   → item: T
             *   Map<K,V>   → item: Json (entry), or k: K, v: V
             *   Set<T>     → item: T
             *   Enum       → item: EnumValue
             *   Range      → item: int
             *   Channel<T> → item: T
             *   string     → item: string
             *   other      → item: unknown
             */
            XrType *item_type = xr_type_new_unknown(NULL);
            XrType *value_type = xr_type_new_unknown(NULL);

            if (is_enum_iter) {
                item_type = xr_type_new_enum(ctx->analyzer->isolate, enum_name);
            } else if (fi->collection && fi->collection->type == AST_RANGE) {
                item_type = xr_type_new_int(NULL);
            } else if (coll_type) {
                if (XR_TYPE_IS_ARRAY(coll_type)) {
                    if (coll_type->container.element_type) {
                        item_type = coll_type->container.element_type;
                    }
                    if (fi->is_keyvalue) {
                        value_type = item_type;
                        item_type = xr_type_new_int(NULL);  // key is index
                    }
                } else if (XR_TYPE_IS_MAP(coll_type)) {
                    if (fi->is_keyvalue) {
                        item_type = coll_type->map.key_type ? coll_type->map.key_type
                                                            : xr_type_new_unknown(NULL);
                        value_type = coll_type->map.value_type ? coll_type->map.value_type
                                                               : xr_type_new_unknown(NULL);
                    } else if (coll_type->map.key_type) {
                        item_type = coll_type->map.key_type;
                    }
                } else if (coll_type->kind == XR_KIND_SET) {
                    if (coll_type->container.element_type) {
                        item_type = coll_type->container.element_type;
                    }
                } else if (coll_type->kind == XR_KIND_CHANNEL) {
                    if (coll_type->container.element_type) {
                        item_type = coll_type->container.element_type;
                    }
                } else if (XR_TYPE_IS_STRING(coll_type)) {
                    item_type = xr_type_new_string(NULL);
                    if (fi->is_keyvalue) {
                        value_type = xr_type_new_string(NULL);
                        item_type = xr_type_new_int(NULL);  // key is index
                    }
                } else if (XR_TYPE_IS_TUPLE(coll_type)) {
                    /* Tuples are heterogeneous by design — there is no
                     * single element type. Iteration would either widen
                     * to a useless union or reach for `any`, neither of
                     * which is acceptable in xray. The user wants .N /
                     * destructuring / pattern matching, not for-in. */
                    XrLocation loc = {
                        .file = ctx->file_path, .line = node->line, .column = node->column};
                    char msg[256];
                    snprintf(msg, sizeof(msg),
                             "tuple type '%s' is not iterable; use '.0', '.1' or destructure "
                             "with 'let (a, b, ...) = t' instead",
                             xr_type_to_string(coll_type));
                    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                               XR_ERR_ANALYZE_TYPE_MISMATCH, msg, &loc);
                } else {
                    /* Anything else (instance of a class / struct, json
                     * literal type, etc.) iterates only via the
                     * iterator() / hasNext() / next() protocol. If the
                     * type doesn't satisfy that contract we emit a
                     * compile-time diagnostic — the alternative is a
                     * runtime "method 'iterator' not found" which is
                     * strictly worse. */
                    XrType *elem = NULL;
                    if (xa_analyzer_is_iterable(ctx->analyzer, coll_type, &elem) && elem) {
                        item_type = elem;
                    } else {
                        XrLocation loc = {
                            .file = ctx->file_path, .line = node->line, .column = node->column};
                        char msg[256];
                        snprintf(msg, sizeof(msg),
                                 "type '%s' is not iterable; expected an Array, Map, Set, string, "
                                 "Json, range or a class with an 'iterator()' method",
                                 xr_type_to_string(coll_type));
                        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                                   XR_ERR_ANALYZE_TYPE_MISMATCH, msg, &loc);
                    }
                }
            }

            // Set item variable type
            if (fi->item_name) {
                XaSymbol *item_sym = xa_scope_lookup(ctx->analyzer->current_scope, fi->item_name);
                if (item_sym) {
                    if (fi->item_symbol_id == 0)
                        fi->item_symbol_id = item_sym->id;
                    XaSymbolLinks *item_links = xa_analyzer_get_links(ctx->analyzer, item_sym);
                    if (item_links)
                        item_links->type = item_type;
                }
            }

            // Cache the inferred item type on the for-in node itself so the
            // IR lowerer can recover heterogeneous element types (e.g. tuple
            // elements in Array<(K,V)>) without re-running inference.
            if (item_type)
                xa_analyzer_set_node_type(ctx->analyzer, node, item_type);

            // Set value variable type (key-value mode)
            if (fi->is_keyvalue && fi->value_name) {
                XaSymbol *val_sym = xa_scope_lookup(ctx->analyzer->current_scope, fi->value_name);
                if (val_sym) {
                    if (fi->value_symbol_id == 0)
                        fi->value_symbol_id = val_sym->id;
                    XaSymbolLinks *val_links = xa_analyzer_get_links(ctx->analyzer, val_sym);
                    if (val_links)
                        val_links->type = value_type;
                }
            }

            // Infer body - process block statements inline (without xa_visit_block_stmt)
            // to match Pass 1 scope structure: Pass 1 processes for-in body block
            // statements in the for-in scope, so Pass 2 must do the same.
            if (fi->body) {
                if (fi->body->type == AST_BLOCK) {
                    BlockNode *blk = &fi->body->as.block;
                    for (int si = 0; si < blk->count; si++) {
                        xa_visit_infer_stmt(ctx, blk->statements[si]);
                    }
                } else {
                    xa_visit_infer_stmt(ctx, fi->body);
                }
            }

            xa_analyzer_exit_scope(ctx->analyzer);
            break;
        }
        case AST_DESTRUCTURE_DECL: {
            DestructureDeclNode *dd = &node->as.destructure_decl;
            // Infer initializer type
            XrType *init_type = dd->initializer ? xa_visit_infer_expr(ctx, dd->initializer)
                                                : xr_type_new_unknown(NULL);

            // Set types on bound variables
            if (dd->pattern) {
                XrDestructurePattern *pat = dd->pattern;
                if (pat->type == PATTERN_ARRAY) {
                    XrType *elem_type = (init_type && XR_TYPE_IS_ARRAY(init_type) &&
                                         init_type->container.element_type)
                                            ? init_type->container.element_type
                                            : xr_type_new_unknown(NULL);
                    for (int i = 0; i < pat->as.array.element_count; i++) {
                        XrDestructurePattern *elem = pat->as.array.elements[i];
                        if (elem && elem->type == PATTERN_IDENTIFIER && elem->as.identifier.name) {
                            XaSymbol *sym = xa_scope_lookup(ctx->analyzer->current_scope,
                                                            elem->as.identifier.name);
                            if (sym) {
                                XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, sym);
                                if (links) {
                                    links->type = elem_type;
                                    links->is_definitely_assigned = true;
                                }
                            }
                        }
                    }
                } else if (pat->type == PATTERN_TUPLE) {
                    /* Tuple destructuring is heterogeneous: each
                     * sub-pattern receives the type at the matching
                     * tuple position. Arity must match exactly — there
                     * is no rest pattern, so a mismatch is always a
                     * static error. Non-tuple init types still bind
                     * the variables to unknown so name-resolution in
                     * the body keeps working after the diagnostic. */
                    int pat_count = pat->as.array.element_count;
                    bool init_is_tuple = init_type && XR_TYPE_IS_TUPLE(init_type);
                    if (init_is_tuple && init_type->tuple.element_count != pat_count) {
                        XrLocation loc = {
                            .file = ctx->file_path, .line = node->line, .column = node->column};
                        char msg[160];
                        snprintf(msg, sizeof(msg),
                                 "tuple pattern has %d element(s) but value has %d", pat_count,
                                 init_type->tuple.element_count);
                        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                                   XR_ERR_ANALYZE_TYPE_MISMATCH, msg, &loc);
                    } else if (init_type && !XR_TYPE_IS_TUPLE(init_type) &&
                               !XR_TYPE_IS_UNKNOWN(init_type)) {
                        XrLocation loc = {
                            .file = ctx->file_path, .line = node->line, .column = node->column};
                        char msg[160];
                        snprintf(msg, sizeof(msg),
                                 "tuple destructuring requires a tuple value, got '%s'",
                                 xr_type_to_string(init_type));
                        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                                   XR_ERR_ANALYZE_TYPE_MISMATCH, msg, &loc);
                    }
                    for (int i = 0; i < pat_count; i++) {
                        XrDestructurePattern *elem = pat->as.array.elements[i];
                        if (elem && elem->type == PATTERN_IDENTIFIER && elem->as.identifier.name) {
                            XaSymbol *sym = xa_scope_lookup(ctx->analyzer->current_scope,
                                                            elem->as.identifier.name);
                            if (sym) {
                                XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, sym);
                                if (links) {
                                    XrType *t =
                                        (init_is_tuple && i < init_type->tuple.element_count)
                                            ? init_type->tuple.element_types[i]
                                            : xr_type_new_unknown(NULL);
                                    links->type = t ? t : xr_type_new_unknown(NULL);
                                    links->is_definitely_assigned = true;
                                }
                            }
                        }
                    }
                } else if (pat->type == PATTERN_OBJECT) {
                    for (int i = 0; i < pat->as.object.field_count; i++) {
                        XrDestructurePattern *vp = pat->as.object.patterns[i];
                        if (vp && vp->type == PATTERN_IDENTIFIER && vp->as.identifier.name) {
                            XaSymbol *sym = xa_scope_lookup(ctx->analyzer->current_scope,
                                                            vp->as.identifier.name);
                            if (sym) {
                                XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, sym);
                                if (links) {
                                    // Try to infer field type from init_type
                                    XrType *field_type = NULL;
                                    const char *field_name = (pat->as.object.field_names &&
                                                              i < pat->as.object.field_count)
                                                                 ? pat->as.object.field_names[i]
                                                                 : vp->as.identifier.name;
                                    if (init_type && field_name) {
                                        field_type =
                                            xr_type_object_get_field(init_type, field_name);
                                    }
                                    links->type =
                                        field_type ? field_type : xr_type_new_unknown(NULL);
                                    links->is_definitely_assigned = true;
                                }
                            }
                        }
                    }
                }
            }
            break;
        }
        case AST_PRINT_STMT:
            for (int i = 0; i < node->as.print_stmt.expr_count; i++) {
                xa_visit_infer_expr(ctx, node->as.print_stmt.exprs[i]);
            }
            break;
        case AST_INDEX_SET: {
            IndexSetNode *is = &node->as.index_set;
            XrType *array_type = NULL;
            XrType *index_type = NULL;
            if (is->array)
                array_type = xa_visit_infer_expr(ctx, is->array);
            if (is->index)
                index_type = xa_visit_infer_expr(ctx, is->index);
            if (is->value)
                xa_visit_infer_expr(ctx, is->value);
            if (array_type && XR_TYPE_IS_JSON(array_type) && array_type->object.field_count > 0 &&
                is->index && is->index->type == AST_LITERAL_STRING) {
                const char *key = is->index->as.literal.raw_value.string_val;
                int field_idx = json_field_index_local(array_type, key);
                XrLocation loc = {
                    .file = ctx->file_path, .line = node->line, .column = node->column};
                if (field_idx >= 0) {
                    bool readonly = array_type->object.field_readonly
                                        ? array_type->object.field_readonly[field_idx]
                                        : false;
                    if (readonly) {
                        char msg[256];
                        snprintf(msg, sizeof(msg), "无法修改只读字段 '%s.%s'（const 修饰）",
                                 json_type_label_local(array_type), key);
                        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                                   XR_ERR_ANALYZE_CONST_ASSIGN, msg, &loc);
                    }
                } else if (array_type->object.is_sealed) {
                    char msg[256];
                    snprintf(msg, sizeof(msg), "类型 '%s' 不允许添加字段 '%s'",
                             json_type_label_local(array_type), key);
                    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                               XR_ERR_ANALYZE_TYPE_MISMATCH, msg, &loc);
                }
            } else if (array_type &&
                       (XR_TYPE_IS_ARRAY(array_type) || XR_TYPE_IS_STRING(array_type)) &&
                       index_type && !XR_TYPE_IS_UNKNOWN(index_type) &&
                       !XR_TYPE_IS_INT(index_type)) {
                XrLocation loc = {
                    .file = ctx->file_path, .line = node->line, .column = node->column};
                char msg[256];
                snprintf(msg, sizeof(msg),
                         "Index type '%s' is not assignable to expected type 'int'",
                         xr_type_to_string(index_type));
                xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                           XR_ERR_ANALYZE_TYPE_MISMATCH, msg, &loc);
            }
            break;
        }
        case AST_DEFER_STMT:
            if (node->as.defer_stmt.expr)
                xa_visit_infer_expr(ctx, node->as.defer_stmt.expr);
            break;
        case AST_SCOPE_BLOCK:
            if (node->as.scope_block.body)
                xa_visit_infer_stmt(ctx, node->as.scope_block.body);
            break;
        case AST_SELECT_STMT: {
            SelectStmtNode *sel = &node->as.select_stmt;
            for (int i = 0; i < sel->case_count; i++) {
                if (sel->cases[i])
                    xa_visit_infer_stmt(ctx, sel->cases[i]);
            }
            break;
        }
        case AST_SELECT_CASE: {
            SelectCaseNode *sc = &node->as.select_case;
            if (sc->channel)
                xa_visit_infer_expr(ctx, sc->channel);
            if (sc->value)
                xa_visit_infer_expr(ctx, sc->value);
            /* Recv cases have a block scope from pass 1 for the variable. */
            if (sc->var_name && !sc->is_send && !sc->is_default) {
                xa_analyzer_enter_scope(ctx->analyzer, XA_SCOPE_BLOCK, node);
                if (sc->body) {
                    if (sc->body->type == AST_BLOCK) {
                        BlockNode *blk = &sc->body->as.block;
                        for (int si = 0; si < blk->count; si++)
                            xa_visit_infer_stmt(ctx, blk->statements[si]);
                    } else {
                        xa_visit_infer_stmt(ctx, sc->body);
                    }
                }
                xa_analyzer_exit_scope(ctx->analyzer);
            } else {
                if (sc->body)
                    xa_visit_infer_stmt(ctx, sc->body);
            }
            break;
        }
        case AST_DESTRUCTURE_ASSIGN: {
            DestructureAssignNode *da = &node->as.destructure_assign;
            if (da->value)
                xa_visit_infer_expr(ctx, da->value);
            /* Resolve symbol_ids on pattern target identifiers so the
             * lowerer can find existing variables via Braun SSA. */
            if (da->pattern) {
                XrDestructurePattern *pat = da->pattern;
                if (pat->type == PATTERN_ARRAY || pat->type == PATTERN_TUPLE) {
                    for (int i = 0; i < pat->as.array.element_count; i++) {
                        XrDestructurePattern *elem = pat->as.array.elements[i];
                        if (elem && elem->type == PATTERN_IDENTIFIER && elem->as.identifier.name) {
                            XaSymbol *sym = xa_scope_lookup(ctx->analyzer->current_scope,
                                                            elem->as.identifier.name);
                            if (sym)
                                elem->as.identifier.symbol_id = sym->id;
                        }
                    }
                } else if (pat->type == PATTERN_OBJECT) {
                    for (int i = 0; i < pat->as.object.field_count; i++) {
                        XrDestructurePattern *vp = pat->as.object.patterns[i];
                        if (vp && vp->type == PATTERN_IDENTIFIER && vp->as.identifier.name) {
                            XaSymbol *sym = xa_scope_lookup(ctx->analyzer->current_scope,
                                                            vp->as.identifier.name);
                            if (sym)
                                vp->as.identifier.symbol_id = sym->id;
                        }
                    }
                }
            }
            break;
        }
        case AST_BREAK_STMT:
        case AST_CONTINUE_STMT:
        case AST_YIELD_STMT:
        case AST_IMPORT_STMT:
        case AST_ENUM_DECL:
        case AST_INTERFACE_DECL:
            break;
        default:
            /* Expressions used as statements (go, await, call, etc.)
             * fall through to xa_visit_infer_expr. */
            xa_visit_infer_expr(ctx, node);
            break;
    }
}

/* ============================================================================
 * Main Entry Point
 * ============================================================================
 * xa_analyze_ast() runs the full analysis pipeline:
 *   Pass 1   -> Symbol collection
 *   Pass 1.5 -> Class inheritance linking
 *   Pass 2   -> Type inference and checking
 * ========================================================================== */

void xa_analyze_ast(XaAnalyzer *analyzer, AstNode *ast) {
    if (!analyzer || !ast)
        return;

    XaInferContext *ctx = xa_infer_context_new(analyzer);
    if (!ctx)
        return;

    // Pass 1: Collect all symbols
    xa_visit_collect(ctx, ast);

    // Pass 1.5: Link class inheritance chains
    xa_link_class_inheritance(analyzer);

    // Pass 2: Infer types
    xa_visit_infer(ctx, ast);

    xa_infer_context_free(ctx);
}
