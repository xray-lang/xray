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
#include "../../base/xchecks.h"
#include "../../runtime/value/xstruct_layout.h"


// Forward declarations now in xanalyzer_visitor_internal.h

/*
 * Recursively convert XR_KIND_CLASS types whose class_name matches a declared
 * type parameter name into XR_KIND_TYPE_PARAM.  The parser has no knowledge of
 * generic scopes, so `T` in `fn add<T>(item: T)` is parsed as CLASS("T").
 * This fixup must run before the function/method type is finalised.
 */
static XrType *resolve_class_to_type_param(XrayIsolate *X, XrType *type,
                                            const char **tp_names, int tp_count) {
    if (!type || tp_count <= 0) return type;

    // Direct match: CLASS("T") → TYPE_PARAM("T")
    if (type->kind == XR_KIND_CLASS && type->instance.class_name) {
        for (int i = 0; i < tp_count; i++) {
            if (tp_names[i] && strcmp(type->instance.class_name, tp_names[i]) == 0)
                return xr_type_new_type_param(X, tp_names[i], i);
        }
    }

    // Recurse into containers
    if (type->kind == XR_KIND_ARRAY && type->container.element_type) {
        XrType *e = resolve_class_to_type_param(X, type->container.element_type, tp_names, tp_count);
        if (e != type->container.element_type) return xr_type_new_array(X, e);
    }
    if (type->kind == XR_KIND_MAP) {
        XrType *k = resolve_class_to_type_param(X, type->map.key_type, tp_names, tp_count);
        XrType *v = resolve_class_to_type_param(X, type->map.value_type, tp_names, tp_count);
        if (k != type->map.key_type || v != type->map.value_type) return xr_type_new_map(X, k, v);
    }
    if (XR_TYPE_IS_FUNCTION(type)) {
        bool changed = false;
        int pc = type->function.param_count;
        XrType **np = pc > 0 ? xr_malloc(sizeof(XrType*) * pc) : NULL;
        for (int i = 0; i < pc; i++) {
            np[i] = resolve_class_to_type_param(X, type->function.param_types[i], tp_names, tp_count);
            if (np[i] != type->function.param_types[i]) changed = true;
        }
        XrType *ret = resolve_class_to_type_param(X, type->function.return_type, tp_names, tp_count);
        if (ret != type->function.return_type) changed = true;
        if (changed) {
            XrType *ft = xr_type_new_function(X, np, pc, ret, type->function.is_variadic);
            ft->function.min_params = type->function.min_params;
            xr_free(np);
            return ft;
        }
        if (np) xr_free(np);
    }
    // Generic instance: Box<T> etc.
    if (type->kind == XR_KIND_INSTANCE && type->instance.type_arg_count > 0) {
        bool changed = false;
        int ac = type->instance.type_arg_count;
        XrType **na = xr_malloc(sizeof(XrType*) * ac);
        for (int i = 0; i < ac; i++) {
            na[i] = resolve_class_to_type_param(X, type->instance.type_args[i], tp_names, tp_count);
            if (na[i] != type->instance.type_args[i]) changed = true;
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

// Check null→T and T?→T assignment errors.
// Returns true if an error was emitted (caller should still call xr_type_assignable).
bool xa_check_null_safety(XaAnalyzer *analyzer, XrType *target, XrType *source,
                                 const char *context_msg, XrLocation *loc) {
    if (!target || !source) return false;
    if (XR_TYPE_IS_UNKNOWN(target) || XR_TYPE_IS_UNKNOWN(source)) return false;

    // Value types (structs) can never be null
    if (XR_TYPE_IS_NULL(source) && target->is_value_type) {
        char msg[256];
        snprintf(msg, sizeof(msg),
            "%s: cannot assign 'null' to value type '%s' (structs are non-nullable)",
            context_msg, xr_type_to_string(target));
        xa_analyzer_add_diagnostic(analyzer, XR_DIAG_SEV_ERROR,
            XR_ERR_ANALYZE_TYPE_MISMATCH, msg, loc);
        return true;
    }

    // Null-safety violations are always errors (xray is a strongly-typed language).
    XrDiagSeverity null_sev = XR_DIAG_SEV_ERROR;

    // null → T (non-nullable target)
    if (XR_TYPE_IS_NULL(source) && !target->is_nullable) {
        char msg[256];
        snprintf(msg, sizeof(msg),
            "%s: cannot assign 'null' to non-nullable type '%s'",
            context_msg, xr_type_to_string(target));
        xa_analyzer_add_diagnostic(analyzer, null_sev,
            XR_ERR_ANALYZE_TYPE_MISMATCH, msg, loc);
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
            xa_analyzer_add_diagnostic(analyzer, null_sev,
                XR_ERR_ANALYZE_TYPE_MISMATCH, msg, loc);
            return true;
        }
    }

    return false;
}

// Helper: recursively extract a type parameter from an argument type
// e.g., match(Array<T>, Array<int>, "T") -> int
//        match(Map<K,V>, Map<string,int>, "K") -> string
XrType *xa_infer_type_param_from_arg(XrType *param_type, XrType *arg_type,
                                             const char *tp_name, int depth) {
    if (!param_type || !arg_type || depth > 8) return NULL;

    // Direct match: T vs int -> T = int
    if ((param_type->kind == XR_KIND_TYPE_PARAM) &&
        param_type->type_param.name &&
        strcmp(param_type->type_param.name, tp_name) == 0) {
        return arg_type;
    }

    // Array<T> vs Array<int> -> T = int
    if (XR_TYPE_IS_ARRAY(param_type) && XR_TYPE_IS_ARRAY(arg_type)) {
        return xa_infer_type_param_from_arg(
            param_type->container.element_type,
            arg_type->container.element_type, tp_name, depth + 1);
    }

    // Set<T> vs Set<int> -> T = int
    if ((param_type->kind == XR_KIND_SET) && (arg_type->kind == XR_KIND_SET)) {
        return xa_infer_type_param_from_arg(
            param_type->container.element_type,
            arg_type->container.element_type, tp_name, depth + 1);
    }

    // Map<K, V> vs Map<string, int> -> K = string, V = int
    if (XR_TYPE_IS_MAP(param_type) && XR_TYPE_IS_MAP(arg_type)) {
        XrType *from_key = xa_infer_type_param_from_arg(
            param_type->map.key_type, arg_type->map.key_type, tp_name, depth + 1);
        if (from_key) return from_key;
        return xa_infer_type_param_from_arg(
            param_type->map.value_type, arg_type->map.value_type, tp_name, depth + 1);
    }

    // Task<T> vs Task<int> -> T = int
    if (xr_type_is_named_class(param_type, "Task") && xr_type_is_named_class(arg_type, "Task")) {
        XrType *pt = (param_type->instance.type_arg_count > 0) ? param_type->instance.type_args[0] : NULL;
        XrType *at = (arg_type->instance.type_arg_count > 0) ? arg_type->instance.type_args[0] : NULL;
        return xa_infer_type_param_from_arg(pt, at, tp_name, depth + 1);
    }

    // Channel<T> vs Channel<int> -> T = int
    if ((param_type->kind == XR_KIND_CHANNEL) && (arg_type->kind == XR_KIND_CHANNEL)) {
        return xa_infer_type_param_from_arg(
            param_type->container.element_type,
            arg_type->container.element_type, tp_name, depth + 1);
    }

    // fn(T): U vs fn(int): string -> T = int, U = string
    if (XR_TYPE_IS_FUNCTION(param_type) && XR_TYPE_IS_FUNCTION(arg_type)) {
        int pc = param_type->function.param_count;
        int ac = arg_type->function.param_count;
        int min = pc < ac ? pc : ac;
        for (int i = 0; i < min; i++) {
            XrType *r = xa_infer_type_param_from_arg(
                param_type->function.param_types[i],
                arg_type->function.param_types[i], tp_name, depth + 1);
            if (r) return r;
        }
        if (param_type->function.return_type && arg_type->function.return_type) {
            return xa_infer_type_param_from_arg(
                param_type->function.return_type,
                arg_type->function.return_type, tp_name, depth + 1);
        }
    }

    return NULL;
}

// Helper: apply generic type substitution for a call expression
// Builds param_names from symbol links, resolves actual types (explicit or inferred),
// then substitutes into return_type. Returns the substituted type.
XrType *xa_substitute_generic_call(XaInferContext *ctx,
                                           XaSymbolLinks *links,
                                           XrType *callee_type,
                                           XrType *return_type,
                                           CallExprNode *call,
                                           int arg_count) {
    XR_DCHECK(ctx != NULL, "substitute_generic_call: NULL ctx");
    XR_DCHECK(links != NULL, "substitute_generic_call: NULL links");
    int type_param_count = xa_symbol_links_get_type_param_count(links);
    if (type_param_count <= 0 || !return_type) return return_type;

    const char **param_names = xr_malloc(sizeof(const char*) * type_param_count);
    if (!param_names) return return_type;
    for (int i = 0; i < type_param_count; i++) {
        param_names[i] = xa_symbol_links_get_type_param_name(links, i);
    }

    XrType **actual_types = NULL;
    int actual_count = 0;
    bool inferred = false;

    if (call->type_arg_count > 0) {
        actual_types = call->type_args;
        actual_count = call->type_arg_count;
    } else {
        actual_types = xr_malloc(sizeof(XrType*) * type_param_count);
        if (!actual_types) { xr_free(param_names); return return_type; }
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
                XrType *at = call->arguments[j]->compile_type
                           ? call->arguments[j]->compile_type
                           : xa_visit_infer_expr(ctx, call->arguments[j]);
                if (pt && at) {
                    actual_types[i] = xa_infer_type_param_from_arg(pt, at, tp_name, 0);
                    if (actual_types[i]) break;
                }
            }
        }
    }

    if (actual_count > 0) {
        return_type = xr_type_substitute(ctx->analyzer->isolate, return_type, param_names, actual_types, actual_count);
    }

    xr_free(param_names);
    if (inferred && actual_types) xr_free(actual_types);
    return return_type;
}

// Check if a function body contains any 'return <expr>' (non-void return).
// Does NOT recurse into nested functions/lambdas.
XR_FUNC bool xa_body_has_return_expr(AstNode *node) {
    if (!node) return false;
    switch (node->type) {
        case AST_RETURN_STMT: {
            ReturnStmtNode *ret = &node->as.return_stmt;
            return (ret->value_count > 0 && ret->values && ret->values[0]);
        }
        case AST_BLOCK: {
            BlockNode *block = &node->as.block;
            for (int i = 0; i < block->count; i++) {
                if (xa_body_has_return_expr(block->statements[i])) return true;
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
                    if (xa_body_has_return_expr(m->arms[i]->as.match_arm.body)) return true;
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

// Recursively collect all return types from a statement tree
static void collect_return_types(XaInferContext *ctx, AstNode *node,
                                 XrType ***types, int *count, int *cap) {
    XR_DCHECK(ctx != NULL, "collect_return_types: NULL ctx");
    if (!node) return;

    switch (node->type) {
        case AST_RETURN_STMT: {
            ReturnStmtNode *ret = &node->as.return_stmt;
            XrType *rt = xr_type_new_void(NULL);
            if (ret->value_count == 1 && ret->values && ret->values[0]) {
                rt = xa_visit_infer(ctx, ret->values[0]);
            } else if (ret->value_count > 1) {
                XrType **elems = xr_malloc(sizeof(XrType*) * ret->value_count);
                for (int i = 0; i < ret->value_count; i++) {
                    elems[i] = ret->values[i] ? xa_visit_infer(ctx, ret->values[i]) : xr_type_new_unknown(NULL);
                }
                rt = xr_type_new_tuple(ctx->analyzer->isolate, elems, ret->value_count);
                xr_free(elems);
            }
            // Add to collected types
            if (*count >= *cap) {
                *cap = *cap ? *cap * 2 : 8;
                *types = xr_realloc(*types, sizeof(XrType*) * (*cap));
            }
            (*types)[(*count)++] = rt;
            break;
        }
        case AST_BLOCK: {
            BlockNode *block = &node->as.block;
            for (int i = 0; i < block->count; i++) {
                collect_return_types(ctx, block->statements[i], types, count, cap);
            }
            break;
        }
        case AST_IF_STMT:
            collect_return_types(ctx, node->as.if_stmt.then_branch, types, count, cap);
            collect_return_types(ctx, node->as.if_stmt.else_branch, types, count, cap);
            break;
        case AST_WHILE_STMT:
            collect_return_types(ctx, node->as.while_stmt.body, types, count, cap);
            break;
        case AST_FOR_STMT:
            collect_return_types(ctx, node->as.for_stmt.body, types, count, cap);
            break;
        case AST_FOR_IN_STMT:
            collect_return_types(ctx, node->as.for_in_stmt.body, types, count, cap);
            break;
        case AST_TRY_CATCH:
            collect_return_types(ctx, node->as.try_catch.try_body, types, count, cap);
            collect_return_types(ctx, node->as.try_catch.catch_body, types, count, cap);
            // finally return is NOT collected: a return inside finally overrides the
            // try/catch return value entirely, so it must not be unioned with them.
            break;
        case AST_MATCH_EXPR: {
            MatchExprNode *m = &node->as.match_expr;
            for (int i = 0; i < m->arm_count; i++) {
                if (m->arms[i] && m->arms[i]->type == AST_MATCH_ARM) {
                    collect_return_types(ctx, m->arms[i]->as.match_arm.body, types, count, cap);
                }
            }
            break;
        }
        default:
            break;
    }
}

// Infer return type by scanning all return statements in function/method body
XrType *xa_infer_function_return_type(XaInferContext *ctx, AstNode *body) {
    if (!body) return NULL;

    XrType **types = NULL;
    int count = 0, cap = 0;
    collect_return_types(ctx, body, &types, &count, &cap);

    if (count == 0) {
        if (types) xr_free(types);
        return NULL;
    }

    // Union all collected return types
    XrType *result = types[0];
    for (int i = 1; i < count; i++) {
        if (!xr_type_equals(result, types[i])) {
            result = xr_type_union(ctx->analyzer->isolate, result, types[i]);
        }
    }

    xr_free(types);
    return result;
}

/* ============================================================================
 * Pass 1: Symbol Collection
 * ============================================================================
 * Collects all symbols (functions, classes, variables) before type inference.
 * Uses two-phase approach for functions/classes to support mutual recursion.
 * ========================================================================== */

// Forward declaration
static void xa_visit_collect_function_decl_only(XaInferContext *ctx, AstNode *node);
static void xa_visit_collect_function_body(XaInferContext *ctx, AstNode *node);

// Helper: collect statements with function hoisting (two-phase)
static void xa_visit_collect_statements_with_hoisting(XaInferContext *ctx, AstNode **stmts, int count) {
    // Phase 1: Collect all function/class/enum declarations first (hoisting)
    for (int i = 0; i < count; i++) {
        AstNode *stmt = stmts[i];
        if (!stmt) continue;
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
        if (!stmt) continue;
        if (stmt->type == AST_FUNCTION_DECL) {
            xa_visit_collect_function_body(ctx, stmt);
        } else if (stmt->type == AST_EXPORT_STMT && stmt->as.export_stmt.declaration) {
            AstNode *decl = stmt->as.export_stmt.declaration;
            if (decl->type == AST_FUNCTION_DECL) {
                xa_visit_collect_function_body(ctx, decl);
            } else if (decl->type != AST_CLASS_DECL && decl->type != AST_STRUCT_DECL && decl->type != AST_ENUM_DECL) {
                xa_visit_collect(ctx, decl);
            }
        } else if (stmt->type != AST_CLASS_DECL && stmt->type != AST_STRUCT_DECL && stmt->type != AST_ENUM_DECL) {
            // Classes and enums already handled in phase 1
            xa_visit_collect(ctx, stmt);
        }
    }
}

// Helper: collect import statement (register module variable in symbol table)
static void xa_visit_collect_import(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node) return;

    ImportStmtNode *import = &node->as.import_stmt;

    // For whole module import: import math or import math as m
    if (import->member_count == 0) {
        const char *var_name = import->alias ? import->alias : import->module_name;

        XaSymbol *sym = xa_symbol_new(var_name, XA_SYM_MODULE);
        if (sym) {
            sym->location.line = node->line;
            xa_scope_add_symbol(ctx->analyzer->current_scope, sym);

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

                XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, sym);
                if (links) {
                    // Try to resolve type from module signatures
                    XrType *member_type = NULL;
                    const char *sig = xa_builtin_get_module_func_signature(
                        import->module_name, member->name);
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
    if (!ctx || !node) return;

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
                XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, sym);
                links->type = xr_type_new_enum(ctx->analyzer->isolate, edecl->name);
                links->declared_type = links->type;

                // Store enum member names for exhaustiveness checking
                if (edecl->member_count > 0) {
                    links->enum_member_names = xr_malloc(sizeof(const char*) * edecl->member_count);
                    links->enum_member_count = 0;
                    for (int m = 0; m < edecl->member_count; m++) {
                        AstNode *mem = edecl->members[m];
                        if (mem && mem->type == AST_ENUM_MEMBER && mem->as.enum_member.name) {
                            links->enum_member_names[links->enum_member_count++] = mem->as.enum_member.name;
                        }
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
                sym->alias_type = node->compile_type;
                xa_scope_add_symbol(ctx->analyzer->current_scope, sym);

                XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, sym);
                if (links) {
                    links->type = node->compile_type ? node->compile_type : xr_type_new_unknown(NULL);
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
                xa_scope_add_symbol(ctx->analyzer->current_scope, sym);
                XaSymbolLinks *item_links = xa_analyzer_get_links(ctx->analyzer, sym);
                if (item_links) item_links->is_definitely_assigned = true;
            }
            if (fi->is_keyvalue && fi->value_name) {
                XaSymbol *vsym = xa_symbol_new(fi->value_name, XA_SYM_VARIABLE);
                vsym->location.line = node->line;
                xa_scope_add_symbol(ctx->analyzer->current_scope, vsym);
                XaSymbolLinks *val_links = xa_analyzer_get_links(ctx->analyzer, vsym);
                if (val_links) val_links->is_definitely_assigned = true;
            }
            if (fi->body) {
                xa_visit_collect(ctx, fi->body);
            }
            xa_analyzer_exit_scope(ctx->analyzer);
            break;
        }
        case AST_BLOCK:
            // Use hoisting for blocks too (supports mutual recursion in nested functions)
            xa_visit_collect_statements_with_hoisting(ctx,
                node->as.block.statements, node->as.block.count);
            break;

        // P1-1: Recurse into control flow statements to collect nested declarations
        case AST_FOR_STMT: {
            ForStmtNode *fs = &node->as.for_stmt;
            xa_analyzer_enter_scope(ctx->analyzer, XA_SCOPE_BLOCK, node);
            if (fs->initializer) xa_visit_collect(ctx, fs->initializer);
            if (fs->body) xa_visit_collect(ctx, fs->body);
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
            if (tc->try_body) xa_visit_collect(ctx, tc->try_body);
            if (tc->catch_body) {
                xa_analyzer_enter_scope(ctx->analyzer, XA_SCOPE_BLOCK, node);
                if (tc->catch_var) {
                    XaSymbol *err_sym = xa_symbol_new(tc->catch_var, XA_SYM_VARIABLE);
                    err_sym->location.line = tc->catch_var_line;
                    xa_scope_add_symbol(ctx->analyzer->current_scope, err_sym);
                    XaSymbolLinks *err_links = xa_analyzer_get_links(ctx->analyzer, err_sym);
                    if (err_links) {
                        err_links->type = xr_type_new_named_instance(ctx->analyzer->isolate, "Exception");
                        err_links->is_definitely_assigned = true;
                    }
                }
                xa_visit_collect(ctx, tc->catch_body);
                xa_analyzer_exit_scope(ctx->analyzer);
            }
            if (tc->finally_body) xa_visit_collect(ctx, tc->finally_body);
            break;
        }
        case AST_EXPR_STMT:
            // Recurse into expression statements (may contain function expressions etc.)
            break;

        // P1-2: Collect destructuring and multi-var declarations
        case AST_DESTRUCTURE_DECL: {
            DestructureDeclNode *dd = &node->as.destructure_decl;
            if (dd->pattern) {
                XrDestructurePattern *pat = dd->pattern;
                if (pat->type == PATTERN_ARRAY) {
                    for (int i = 0; i < pat->as.array.element_count; i++) {
                        XrDestructurePattern *elem = pat->as.array.elements[i];
                        if (elem && elem->type == PATTERN_IDENTIFIER && elem->as.identifier.name) {
                            XaSymbol *sym = xa_symbol_new(elem->as.identifier.name, XA_SYM_VARIABLE);
                            sym->is_const = dd->is_const;
                            sym->location.line = node->line;
                            xa_scope_add_symbol(ctx->analyzer->current_scope, sym);
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
                        }
                    }
                }
            }
            break;
        }
        case AST_MULTI_VAR_DECL: {
            MultiVarDeclNode *mv = &node->as.multi_var_decl;
            for (int i = 0; i < mv->name_count; i++) {
                if (mv->names[i]) {
                    XaSymbol *sym = xa_symbol_new(mv->names[i], XA_SYM_VARIABLE);
                    sym->is_const = mv->is_const;
                    sym->location.line = node->line;
                    xa_scope_add_symbol(ctx->analyzer->current_scope, sym);
                }
            }
            break;
        }

        default:
            break;
    }
}

void xa_visit_collect_program(XaInferContext *ctx, AstNode *node) {
    if (!node || node->type != AST_PROGRAM) return;

    ProgramNode *prog = &node->as.program;
    // Use hoisting to support mutual recursion at module level
    xa_visit_collect_statements_with_hoisting(ctx, prog->statements, prog->count);
}

// Phase 1: Collect function declaration only (symbol, not body)
static void xa_visit_collect_function_decl_only(XaInferContext *ctx, AstNode *node) {
    if (!node) return;

    FunctionDeclNode *fn = &node->as.function_decl;

    // Create function symbol
    XaSymbol *sym = xa_symbol_new(fn->name, XA_SYM_FUNCTION);
    sym->location.line = node->line;

    // Build function type and collect param names
    XrType **param_types = NULL;
    const char **param_names = NULL;
    bool has_rest = false;

    if (fn->param_count > 0) {
        param_types = xr_malloc(sizeof(XrType*) * fn->param_count);
        param_names = xr_malloc(sizeof(const char*) * fn->param_count);
        if (!param_types || !param_names) {
            xr_free(param_types);
            xr_free(param_names);
            return;
        }
        for (int i = 0; i < fn->param_count; i++) {
            XrParamNode *param = fn->params[i];
            param_types[i] = (param && param->type) ?
                (XrType*)param->type : xr_type_new_unknown(NULL);
            param_names[i] = param ? param->name : NULL;
            if (param && param->is_rest) has_rest = true;

            // Warn: function parameter missing type annotation
            if (param && !param->type && !param->is_rest) {
                char msg[256];
                snprintf(msg, sizeof(msg),
                    "Parameter '%s' of function '%s' is missing type annotation",
                    param->name, fn->name ? fn->name : "<anonymous>");
                XrLocation loc = { .file = ctx->file_path, .line = param->line, .column = param->column };
                xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                    XR_ERR_ANALYZE_MISSING_TYPE, msg, &loc);
            }
        }
    }

    // Omitted return type defaults to void; error if body has 'return <expr>'
    XrType *return_type = fn->return_type ? (XrType*)fn->return_type : xr_type_new_void(NULL);
    if (!fn->return_type && fn->name && fn->body) {
        if (xa_body_has_return_expr(fn->body)) {
            char msg[256];
            snprintf(msg, sizeof(msg),
                "Function '%s' returns a value but has no return type annotation",
                fn->name);
            XrLocation loc = { .file = ctx->file_path, .line = node->line, .column = node->column };
            xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                XR_ERR_ANALYZE_MISSING_TYPE, msg, &loc);
        }
    }
    // Resolve CLASS("T") → TYPE_PARAM("T") for generic functions
    if (fn->type_param_count > 0 && fn->type_params) {
        const char *tp_buf[8];
        const char **tp_names = (fn->type_param_count <= 8)
            ? tp_buf : xr_malloc(sizeof(const char*) * fn->type_param_count);
        for (int i = 0; i < fn->type_param_count; i++)
            tp_names[i] = fn->type_params[i]->name;
        for (int i = 0; i < fn->param_count; i++)
            param_types[i] = resolve_class_to_type_param(NULL, param_types[i], tp_names, fn->type_param_count);
        return_type = resolve_class_to_type_param(NULL, return_type, tp_names, fn->type_param_count);
        if (tp_names != tp_buf) xr_free((void*)tp_names);
    }

    XrType *fn_type = xr_type_new_function(ctx->analyzer->isolate, param_types, fn->param_count, return_type, has_rest);

    // Set min_params for default parameter support
    if (fn_type) {
        fn_type->function.min_params = fn->required_count;
    }

    // Add to scope
    xa_scope_add_symbol(ctx->analyzer->current_scope, sym);

    // Create symbol links with type and param names
    XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, sym);
    links->type = fn_type;
    links->declared_type = fn_type;
    links->file_path = ctx->file_path;

    // Store parameter names for LSP inlay hints
    xa_symbol_links_set_function_sig(links, param_types, param_names, fn->param_count, return_type);

    // Store generic type parameters and constraints
    if (fn->type_param_count > 0 && fn->type_params) {
        const char **type_param_names = xr_malloc(sizeof(const char*) * fn->type_param_count);
        XrType **type_param_constraints = xr_malloc(sizeof(XrType*) * fn->type_param_count);

        if (type_param_names && type_param_constraints) {
            for (int i = 0; i < fn->type_param_count; i++) {
                type_param_names[i] = fn->type_params[i]->name;
                type_param_constraints[i] = fn->type_params[i]->constraint;
            }

            xa_symbol_links_set_type_params(links, type_param_names, type_param_constraints,
                                             fn->type_param_count);
        }

        xr_free(type_param_names);
        xr_free(type_param_constraints);
    }

    if (param_types) xr_free(param_types);
    if (param_names) xr_free(param_names);
}

// Collect return-value AST nodes from a function body.
// Only collects object-literal returns; sets out_bad if a non-object, non-null return is found.
static void xa_collect_returns(AstNode *node, AstNode **out, int *count, int cap, bool *out_bad) {
    if (!node || *out_bad) return;
    switch (node->type) {
        case AST_BLOCK: {
            BlockNode *blk = &node->as.block;
            for (int i = 0; i < blk->count; i++)
                xa_collect_returns(blk->statements[i], out, count, cap, out_bad);
            break;
        }
        case AST_IF_STMT: {
            IfStmtNode *ifn = &node->as.if_stmt;
            xa_collect_returns(ifn->then_branch, out, count, cap, out_bad);
            if (ifn->else_branch)
                xa_collect_returns(ifn->else_branch, out, count, cap, out_bad);
            break;
        }
        case AST_RETURN_STMT: {
            ReturnStmtNode *ret = &node->as.return_stmt;
            if (ret->value_count == 0) break;
            AstNode *val = ret->values[0];
            if (val->type == AST_LITERAL_NULL) break;
            if (val->type == AST_OBJECT_LITERAL) {
                if (*count < cap) out[(*count)++] = val;
            } else {
                *out_bad = true;
            }
            break;
        }
        default: break;
    }
}

// Infer Json return type for a function whose returns are all same-shape object literals.
// Returns an interned XrType (XR_KIND_JSON, allow_extension=true) or NULL.
static XrType *xa_infer_return_json_type(XrayIsolate *X, FunctionDeclNode *fn) {
    if (!fn->body || fn->return_type) return NULL;

    static const int MAX_RETURNS = 32;
    static const int MAX_FIELDS  = 32;
    AstNode *rets[32];
    int nrets = 0;
    bool bad = false;
    xa_collect_returns(fn->body, rets, &nrets, MAX_RETURNS, &bad);
    if (bad || nrets == 0) return NULL;

    ObjectLiteralNode *first = &rets[0]->as.object_literal;
    int fc = 0;
    for (int i = 0; i < first->count; i++) {
        if ((!first->computed || !first->computed[i]) &&
            first->keys[i]->type == AST_LITERAL_STRING)
            fc++;
    }
    if (fc == 0 || fc > MAX_FIELDS) return NULL;

    // Verify all returns have same static field names (order-insensitive)
    for (int r = 1; r < nrets; r++) {
        ObjectLiteralNode *o = &rets[r]->as.object_literal;
        int ofc = 0;
        for (int i = 0; i < o->count; i++)
            if ((!o->computed || !o->computed[i]) && o->keys[i]->type == AST_LITERAL_STRING)
                ofc++;
        if (ofc != fc) return NULL;
        for (int i = 0; i < first->count; i++) {
            if (first->computed && first->computed[i]) continue;
            if (first->keys[i]->type != AST_LITERAL_STRING) continue;
            const char *fname = first->keys[i]->as.literal.raw_value.string_val;
            bool found = false;
            for (int j = 0; j < o->count; j++) {
                if (o->computed && o->computed[j]) continue;
                if (o->keys[j]->type != AST_LITERAL_STRING) continue;
                if (strcmp(o->keys[j]->as.literal.raw_value.string_val, fname) == 0) {
                    found = true; break;
                }
            }
            if (!found) return NULL;
        }
    }

    // Build field name + type arrays from first return's object literal
    const char *names[32];
    XrType *types[32];
    int idx = 0;
    for (int i = 0; i < first->count && idx < 32; i++) {
        if (first->computed && first->computed[i]) continue;
        if (first->keys[i]->type != AST_LITERAL_STRING) continue;
        names[idx] = first->keys[i]->as.literal.raw_value.string_val;
        // Infer field type from AST literal (Pass 1: no full inference available)
        AstNode *val = first->values[i];
        switch (val ? val->type : 0) {
            case AST_LITERAL_INT:    types[idx] = xr_type_new_int(NULL); break;
            case AST_LITERAL_FLOAT:  types[idx] = xr_type_new_float(NULL); break;
            case AST_LITERAL_STRING: types[idx] = xr_type_new_string(NULL); break;
            case AST_LITERAL_TRUE:
            case AST_LITERAL_FALSE:  types[idx] = xr_type_new_bool(NULL); break;
            case AST_LITERAL_NULL:   types[idx] = xr_type_new_unknown(NULL); break;
            default:                 types[idx] = xr_type_new_unknown(NULL); break;
        }
        idx++;
    }
    return xr_type_new_object_anonymous(X, names, types, fc);
}

// Phase 2: Collect function body (parameters and body declarations)
static void xa_visit_collect_function_body(XaInferContext *ctx, AstNode *node) {
    if (!node) return;

    FunctionDeclNode *fn = &node->as.function_decl;

    // Get function type from already-created symbol
    XaSymbol *sym = xa_scope_lookup_local(ctx->analyzer->current_scope, fn->name);
    XaSymbolLinks *links = sym ? xa_analyzer_get_links(ctx->analyzer, sym) : NULL;

    // Enter function scope and collect body
    xa_analyzer_enter_scope(ctx->analyzer, XA_SCOPE_FUNCTION, node);

    // Add parameters to scope
    for (int i = 0; i < fn->param_count; i++) {
        XrParamNode *p = fn->params[i];
        if (p && p->name) {
            XaSymbol *param = xa_symbol_new(p->name, XA_SYM_PARAMETER);
            param->location.line = p->line > 0 ? p->line : node->line;
            param->passing_mode = p->passing_mode;
            xa_scope_add_symbol(ctx->analyzer->current_scope, param);

            XaSymbolLinks *param_links = xa_analyzer_get_links(ctx->analyzer, param);
            if (p->is_rest) {
                // Rest parameter is packed into Array at runtime
                XrType *elem_type = (links && links->param_types && i < links->param_count) ?
                    links->param_types[i] : xr_type_new_unknown(NULL);
                param_links->type = xr_type_new_array(ctx->analyzer->isolate, elem_type);
            } else {
                param_links->type = (links && links->param_types && i < links->param_count) ?
                    links->param_types[i] : xr_type_new_unknown(NULL);
            }
            param_links->is_definitely_assigned = true;

            // Validate in/ref: only struct (value type) parameters allowed
            if (p->passing_mode != XR_PARAM_VALUE && param_links->type) {
                XrType *pt = param_links->type;
                bool is_struct = false;
                if ((pt->kind == XR_KIND_CLASS || pt->kind == XR_KIND_INSTANCE)
                    && pt->instance.class_name) {
                    XaSymbol *csym = xa_analyzer_lookup(ctx->analyzer, pt->instance.class_name);
                    if (csym) {
                        XaSymbolLinks *cl = xa_analyzer_get_links(ctx->analyzer, csym);
                        if (cl && cl->type && cl->type->is_value_type)
                            is_struct = true;
                    }
                }
                if (!is_struct) {
                    const char *mode = (p->passing_mode == XR_PARAM_IN) ? "in" : "ref";
                    XrLocation loc = { .file = ctx->file_path, .line = p->line };
                    char msg[256];
                    snprintf(msg, sizeof(msg),
                        "'%s' modifier on parameter '%s' is only allowed for struct (value) types",
                        mode, p->name);
                    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                        XR_ERR_ANALYZE_TYPE_MISMATCH, msg, &loc);
                }
            }
        }
    }

    // Collect body declarations
    if (fn->body) {
        xa_visit_collect(ctx, fn->body);
    }

    // Infer return type for unannotated functions that always return same-shape objects.
    // This updates the function's return type so that call-site type propagation
    // can see a concrete Json type instead of unknown.
    if (links && !fn->return_type) {
        XrType *inferred_ret = xa_infer_return_json_type(ctx->analyzer->isolate, fn);
        if (inferred_ret) {
            links->return_type = inferred_ret;
            links->return_type_inferred = true;
            // Also update the function type object so xa_visit_call sees it
            if (links->type && XR_TYPE_IS_FUNCTION(links->type)) {
                links->type->function.return_type = inferred_ret;
            }
        }
    }

    xa_analyzer_exit_scope(ctx->analyzer);
}

// Combined: for direct calls (not through hoisting)
void xa_visit_collect_function(XaInferContext *ctx, AstNode *node) {
    xa_visit_collect_function_decl_only(ctx, node);
    xa_visit_collect_function_body(ctx, node);
}

/* ----------------------------------------------------------------------------
 * Constructor super() Validation
 * -------------------------------------------------------------------------- */

// Check if AST node contains 'this' expression (before super() call)
static bool contains_this_expr(AstNode *node) {
    if (!node) return false;

    switch (node->type) {
        case AST_THIS_EXPR:
            return true;
        case AST_CALL_EXPR:
            if (contains_this_expr(node->as.call_expr.callee)) return true;
            for (int i = 0; i < node->as.call_expr.arg_count; i++) {
                if (contains_this_expr(node->as.call_expr.arguments[i])) return true;
            }
            break;
        case AST_MEMBER_ACCESS:
            return contains_this_expr(node->as.member_access.object);
        case AST_INDEX_GET:
            return contains_this_expr(node->as.index_get.array) ||
                   contains_this_expr(node->as.index_get.index);
        case AST_BINARY_ADD: case AST_BINARY_SUB: case AST_BINARY_MUL:
        case AST_BINARY_DIV: case AST_BINARY_MOD: case AST_BINARY_EQ:
        case AST_BINARY_NE: case AST_BINARY_LT: case AST_BINARY_LE:
        case AST_BINARY_GT: case AST_BINARY_GE: case AST_BINARY_AND:
        case AST_BINARY_OR:
            return contains_this_expr(node->as.binary.left) ||
                   contains_this_expr(node->as.binary.right);
        case AST_UNARY_NEG: case AST_UNARY_NOT: case AST_UNARY_BNOT:
            return contains_this_expr(node->as.unary.operand);
        case AST_TERNARY:
            return contains_this_expr(node->as.ternary.condition) ||
                   contains_this_expr(node->as.ternary.true_expr) ||
                   contains_this_expr(node->as.ternary.false_expr);
        case AST_ASSIGNMENT:
            return contains_this_expr(node->as.assignment.value);
        default:
            break;
    }
    return false;
}

// Check if statement contains 'this' expression
static bool stmt_contains_this(AstNode *stmt) {
    if (!stmt) return false;

    switch (stmt->type) {
        case AST_EXPR_STMT:
            return contains_this_expr(stmt->as.expr_stmt);
        case AST_VAR_DECL:
        case AST_CONST_DECL:
            return contains_this_expr(stmt->as.var_decl.initializer);
        case AST_ASSIGNMENT:
            return contains_this_expr(stmt->as.assignment.value);
        case AST_MEMBER_SET:
            return contains_this_expr(stmt->as.member_set.object) ||
                   contains_this_expr(stmt->as.member_set.value);
        case AST_RETURN_STMT:
            for (int i = 0; i < stmt->as.return_stmt.value_count; i++) {
                if (contains_this_expr(stmt->as.return_stmt.values[i])) return true;
            }
            break;
        default:
            break;
    }
    return false;
}

// Validate constructor super() call rules:
// 1. super() must be first statement (if called)
// 2. Cannot access 'this' before super()
// 3. Must call super() if parent has required parameters
static void validate_constructor_super_call(
    XaInferContext *ctx,
    ClassDeclNode *cls,
    MethodDeclNode *constructor,
    AstNode *method_node
) {
    if (!constructor || !constructor->body) return;

    AstNode *body = constructor->body;
    if (body->type != AST_BLOCK) return;

    BlockNode *block = &body->as.block;
    bool has_super_call = false;
    int super_call_index = -1;
    int super_call_line = 0;

    // Find super() call position
    for (int i = 0; i < block->count; i++) {
        AstNode *stmt = block->statements[i];
        if (!stmt) continue;

        // Check for super() call (as expression statement)
        if (stmt->type == AST_EXPR_STMT && stmt->as.expr_stmt) {
            AstNode *expr = stmt->as.expr_stmt;
            if (expr->type == AST_SUPER_CALL) {
                has_super_call = true;
                super_call_index = i;
                super_call_line = stmt->line;
                break;
            }
        }
        // Also check direct super call statement
        if (stmt->type == AST_SUPER_CALL) {
            has_super_call = true;
            super_call_index = i;
            super_call_line = stmt->line;
            break;
        }
    }

    // Check 1: If class has a parent, validate super() usage
    if (cls->super_name) {
        // Check 2: super() must be first statement (if called)
        if (has_super_call && super_call_index > 0) {
            XrLocation loc = { .file = ctx->file_path, .line = super_call_line };
            xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_SUPER_FIRST,
                "super() must be the first statement in constructor", &loc);
        }

        // Check 3: Cannot access 'this' before super()
        if (has_super_call) {
            for (int i = 0; i < super_call_index; i++) {
                AstNode *stmt = block->statements[i];
                if (stmt_contains_this(stmt)) {
                    XrLocation loc = { .file = ctx->file_path, .line = stmt->line };
                    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_SUPER_THIS,
                        "Cannot access 'this' before calling super()", &loc);
                    break;
                }
            }
        }

        // Check 4: Smart super() requirement based on parent constructor
        // - Parent has no constructor → no super() needed
        // - Parent constructor has only optional params → auto-insert at codegen
        // - Parent constructor has required params → must call super(args)
        if (!has_super_call) {
            // Look up parent class info (search outside class scope)
            XaSymbol *parent_sym = xa_scope_lookup(
                ctx->analyzer->current_scope->parent, cls->super_name);
            XrClassInfo *parent_info = NULL;
            if (parent_sym) {
                XaSymbolLinks *parent_links = xa_analyzer_get_links(ctx->analyzer, parent_sym);
                if (parent_links) parent_info = parent_links->class_info;
            }

            if (parent_info && parent_info->has_constructor &&
                parent_info->constructor_required_params > 0) {
                // Parent constructor has required params — must call super()
                XrLocation loc = { .file = ctx->file_path, .line = method_node->line };
                char msg[256];
                snprintf(msg, sizeof(msg),
                    "Constructor must call super() because '%s' constructor requires %d argument(s)",
                    cls->super_name, parent_info->constructor_required_params);
                xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                    XR_ERR_ANALYZE_SUPER_REQUIRED, msg, &loc);
            }
            // else: parent has no constructor or all-optional params → OK
        }
    } else {
        // No parent class - super() should not be called
        if (has_super_call) {
            XrLocation loc = { .file = ctx->file_path, .line = super_call_line };
            xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_SUPER_INVALID,
                "super() can only be called in a class that extends another class", &loc);
        }
    }
}

void xa_visit_collect_class(XaInferContext *ctx, AstNode *node) {
    if (!node) return;

    ClassDeclNode *cls = &node->as.class_decl;

    // Create class symbol
    XaSymbol *sym = xa_symbol_new(cls->name, XA_SYM_CLASS);
    sym->location.line = node->line;

    xa_scope_add_symbol(ctx->analyzer->current_scope, sym);

    // Create class info
    XrClassInfo *info = xa_class_info_new(cls->name);
    if (cls->super_name) {
        info->base_name = xr_strdup(cls->super_name);
    }

    XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, sym);
    links->class_info = info;
    links->type = xr_type_new_class(ctx->analyzer->isolate, cls->name);
    if (node->type == AST_STRUCT_DECL) {
        links->type->is_value_type = true;
    }

    // Store implemented interfaces from 'implements' clause
    if (cls->interface_count > 0 && cls->interfaces) {
        info->interface_names = xr_malloc(sizeof(const char*) * cls->interface_count);
        info->interface_count = cls->interface_count;
        for (int i = 0; i < cls->interface_count; i++) {
            info->interface_names[i] = cls->interfaces[i];
        }
    }

    // Store generic type parameters for the class
    if (cls->type_param_count > 0 && cls->type_params) {
        const char **type_param_names = xr_malloc(sizeof(const char*) * cls->type_param_count);
        XrType **type_param_constraints = xr_malloc(sizeof(XrType*) * cls->type_param_count);

        for (int i = 0; i < cls->type_param_count; i++) {
            type_param_names[i] = cls->type_params[i]->name;
            type_param_constraints[i] = cls->type_params[i]->constraint;
        }

        xa_symbol_links_set_type_params(links, type_param_names, type_param_constraints,
                                         cls->type_param_count);

        xr_free(type_param_names);
        xr_free(type_param_constraints);
    }

    // Enter class scope
    xa_analyzer_enter_scope(ctx->analyzer, XA_SCOPE_CLASS, node);
    ctx->analyzer->current_scope->class_symbol = sym;

    // Collect fields
    for (int i = 0; i < cls->field_count; i++) {
        AstNode *field = cls->fields[i];
        if (field && field->type == AST_FIELD_DECL) {
            FieldDeclNode *fd = &field->as.field_decl;
            XaSymbol *field_sym = xa_symbol_new(fd->name, XA_SYM_PROPERTY);
            field_sym->location.line = field->line;
            field_sym->is_static = fd->is_static;
            field_sym->is_private = fd->is_private;
            xa_scope_add_symbol(ctx->analyzer->current_scope, field_sym);

            XaSymbolLinks *field_links = xa_analyzer_get_links(ctx->analyzer, field_sym);

            // Try explicit type annotation first
            if (fd->field_type) {
                field_links->type = fd->field_type ? (XrType*)fd->field_type : xr_type_new_unknown(NULL);
            } else if (fd->initializer) {
                // Infer type from initializer
                field_links->type = xa_visit_infer(ctx, fd->initializer);
            } else {
                field_links->type = xr_type_new_unknown(NULL);
                // Warn: class field missing type annotation and initializer
                char msg[256];
                snprintf(msg, sizeof(msg),
                    "Field '%s' is missing type annotation (and has no initializer to infer from)",
                    fd->name ? fd->name : "?");
                XrLocation loc = { .file = ctx->file_path, .line = node->line };
                xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                    XR_ERR_ANALYZE_MISSING_TYPE, msg, &loc);
            }

            xa_class_info_add_field(info, field_sym);
        }
    }

    // Check recursive struct self-reference (direct value embedding)
    if (node->type == AST_STRUCT_DECL && cls->name) {
        for (int i = 0; i < info->field_count; i++) {
            XaSymbol *fs = info->fields[i];
            if (!fs) continue;
            XaSymbolLinks *fl = xa_analyzer_get_links(ctx->analyzer, fs);
            if (!fl || !fl->type) continue;
            XrType *ft = fl->type;
            // Field referencing the same struct → infinite size
            const char *type_name = NULL;
            if ((ft->kind == XR_KIND_CLASS || ft->kind == XR_KIND_INSTANCE)
                && ft->instance.class_name) {
                type_name = ft->instance.class_name;
            }
            if (type_name && strcmp(type_name, cls->name) == 0) {
                XrLocation loc = { .file = ctx->file_path, .line = node->line };
                char msg[256];
                snprintf(msg, sizeof(msg),
                    "Struct '%s' cannot have a field of its own type — "
                    "this creates infinite size. Use a class instead for recursive data",
                    cls->name);
                xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                    XR_ERR_ANALYZE_TYPE_MISMATCH, msg, &loc);
                break;
            }
        }
    }

    // Compute struct layout (VALUE_TYPE only, skip generic templates)
    int struct_type_param_count = (node->type == AST_STRUCT_DECL)
        ? node->as.struct_decl.type_param_count : 0;
    if (node->type == AST_STRUCT_DECL && info->field_count > 0
        && struct_type_param_count == 0) {
        XrStructLayout *layout = xr_calloc(1, sizeof(XrStructLayout));
        layout->field_count = (uint16_t)info->field_count;
        bool layout_valid = true;

        for (int i = 0; i < info->field_count && i < XR_MAX_STRUCT_FIELDS; i++) {
            XaSymbol *fs = info->fields[i];
            if (!fs) { layout_valid = false; break; }
            XaSymbolLinks *fl = xa_analyzer_get_links(ctx->analyzer, fs);
            XrType *ft = (fl && fl->type) ? fl->type : NULL;

            if (!ft || ft->kind == XR_KIND_UNKNOWN) {
                // Phase 1: struct fields must have explicit type annotations
                XrLocation loc = { .file = ctx->file_path, .line = node->line };
                char msg[256];
                snprintf(msg, sizeof(msg),
                    "Struct '%s' field '%s' must have an explicit type annotation "
                    "(int, float, bool, string) — mutable reference types are not supported in struct fields",
                    cls->name ? cls->name : "?", fs->name ? fs->name : "?");
                xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                    XR_ERR_ANALYZE_TYPE_MISMATCH, msg, &loc);
                layout_valid = false;
                break;
            }

            int native = xr_type_kind_to_native(ft->kind, ft->native_width);
            if (native < 0) {
                // Fixed-size array field: [N]T
                if (ft->kind == XR_KIND_FIXED_ARRAY && ft->fixed_array.element_type) {
                    XrType *elem = ft->fixed_array.element_type;
                    int elem_native = xr_type_kind_to_native(elem->kind, elem->native_width);
                    if (elem_native >= 0 && ft->fixed_array.length > 0) {
                        uint32_t field_bytes = (uint32_t)ft->fixed_array.length
                                             * xr_native_type_size((uint8_t)elem_native);
                        if (field_bytes > UINT16_MAX) {
                            XrLocation loc = { .file = ctx->file_path, .line = node->line };
                            char msg[256];
                            snprintf(msg, sizeof(msg),
                                "Fixed array field '%s' in struct '%s' exceeds maximum size "
                                "(%u bytes > 65535). For larger collections, use a class with Array<T>.",
                                fs->name ? fs->name : "?",
                                cls->name ? cls->name : "?",
                                (unsigned)field_bytes);
                            xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                XR_ERR_ANALYZE_TYPE_MISMATCH, msg, &loc);
                            layout_valid = false;
                            break;
                        }
                        layout->fields[i].native_type = XR_NATIVE_ARRAY;
                        layout->fields[i].elem_native_type = (uint8_t)elem_native;
                        layout->fields[i].elem_count = (uint16_t)ft->fixed_array.length;
                    } else {
                        XrLocation loc = { .file = ctx->file_path, .line = node->line };
                        char msg[256];
                        snprintf(msg, sizeof(msg),
                            "Struct '%s' field '%s': fixed array element must be a primitive type",
                            cls->name ? cls->name : "?", fs->name ? fs->name : "?");
                        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                            XR_ERR_ANALYZE_TYPE_MISMATCH, msg, &loc);
                        layout_valid = false;
                        break;
                    }
                    continue;
                }
                // Check if field type is a nested struct with known layout
                const char *field_class_name = NULL;
                if ((ft->kind == XR_KIND_CLASS || ft->kind == XR_KIND_INSTANCE)
                    && ft->instance.class_name) {
                    field_class_name = ft->instance.class_name;
                }
                XrStructLayout *sub_layout = NULL;
                if (field_class_name) {
                    XaSymbol *sub_sym = xa_analyzer_lookup(ctx->analyzer, field_class_name);
                    if (sub_sym) {
                        XaSymbolLinks *sub_links = xa_analyzer_get_links(ctx->analyzer, sub_sym);
                        if (sub_links && sub_links->class_info &&
                            sub_links->class_info->struct_layout) {
                            sub_layout = sub_links->class_info->struct_layout;
                        }
                    }
                }
                if (sub_layout) {
                    // Nested struct: embed with class ptr prefix (8 + data)
                    layout->fields[i].native_type = XR_NATIVE_STRUCT;
                    layout->fields[i].size = 8 + sub_layout->total_size;
                    layout->fields[i].sub_layout_id = sub_layout->layout_id;
                } else {
                    XrLocation loc = { .file = ctx->file_path, .line = node->line };
                    char msg[256];
                    snprintf(msg, sizeof(msg),
                        "Struct '%s' field '%s' has unsupported type — "
                        "only int, float, bool, string, fixed arrays and other structs are supported",
                        cls->name ? cls->name : "?", fs->name ? fs->name : "?");
                    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                        XR_ERR_ANALYZE_TYPE_MISMATCH, msg, &loc);
                    layout_valid = false;
                    break;
                }
                continue;
            }

            layout->fields[i].native_type = (uint8_t)native;
        }

        if (layout_valid && info->field_count <= XR_MAX_STRUCT_FIELDS) {
            xr_struct_layout_compute(layout);
            if (layout->total_size > UINT16_MAX) {
                XrLocation loc = { .file = ctx->file_path, .line = node->line };
                char msg[256];
                snprintf(msg, sizeof(msg),
                    "Struct '%s' total size exceeds maximum (%u bytes > 65535). "
                    "For larger data, use a class with Array<T> fields.",
                    cls->name ? cls->name : "?",
                    (unsigned)layout->total_size);
                xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                    XR_ERR_ANALYZE_TYPE_MISMATCH, msg, &loc);
                xr_free(layout);
            } else {
                info->struct_layout = layout;
            }
        } else {
            xr_free(layout);
        }
    }

    // Collect methods
    for (int i = 0; i < cls->method_count; i++) {
        AstNode *method = cls->methods[i];
        if (method && method->type == AST_METHOD_DECL) {
            MethodDeclNode *md = &method->as.method_decl;
            XaSymbol *method_sym = xa_symbol_new(md->name, XA_SYM_METHOD);
            method_sym->location.line = method->line;
            method_sym->is_static = md->is_static;
            method_sym->is_private = md->is_private;
            xa_scope_add_symbol(ctx->analyzer->current_scope, method_sym);

            // Build method type
            XrType **param_types = NULL;
            const char **param_names = NULL;
            if (md->param_count > 0) {
                param_types = xr_malloc(sizeof(XrType*) * md->param_count);
                param_names = xr_malloc(sizeof(char*) * md->param_count);
                for (int j = 0; j < md->param_count; j++) {
                    param_types[j] = (md->param_types && md->param_types[j]) ?
                        (XrType*)md->param_types[j] : xr_type_new_unknown(NULL);
                    param_names[j] = md->parameters ? md->parameters[j] : NULL;

                    // Validate in/ref: only struct (value type) allowed
                    if (md->param_passing_modes &&
                        md->param_passing_modes[j] != XR_PARAM_VALUE &&
                        param_types[j] && !XR_TYPE_IS_UNKNOWN(param_types[j])) {
                        XrType *pt = param_types[j];
                        bool is_vt = false;
                        if ((pt->kind == XR_KIND_CLASS || pt->kind == XR_KIND_INSTANCE)
                            && pt->instance.class_name) {
                            XaSymbol *csym = xa_analyzer_lookup(ctx->analyzer, pt->instance.class_name);
                            if (csym) {
                                XaSymbolLinks *cl = xa_analyzer_get_links(ctx->analyzer, csym);
                                if (cl && cl->type && cl->type->is_value_type)
                                    is_vt = true;
                            }
                        }
                        if (!is_vt) {
                            const char *mode = (md->param_passing_modes[j] == XR_PARAM_IN) ? "in" : "ref";
                            char msg2[256];
                            snprintf(msg2, sizeof(msg2),
                                "'%s' modifier on parameter '%s' of method '%s' is only allowed for struct (value) types",
                                mode, md->parameters ? md->parameters[j] : "?", md->name ? md->name : "?");
                            XrLocation loc2 = { .file = ctx->file_path, .line = method->line };
                            xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                XR_ERR_ANALYZE_TYPE_MISMATCH, msg2, &loc2);
                        }
                    }

                    // Warn: method parameter missing type annotation (skip constructor)
                    if (!(md->param_types && md->param_types[j]) && !md->is_constructor) {
                        char msg[256];
                        snprintf(msg, sizeof(msg),
                            "Parameter '%s' of method '%s' is missing type annotation",
                            md->parameters ? md->parameters[j] : "?", md->name ? md->name : "?");
                        XrLocation loc = { .file = ctx->file_path, .line = method->line };
                        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                            XR_ERR_ANALYZE_MISSING_TYPE, msg, &loc);
                    }
                }
            }

            // Omitted return type defaults to void; error if body has 'return <expr>'
            // Skip getter/setter (set:xxx, get:xxx) - return types are implicit
            bool is_accessor = md->name && (strncmp(md->name, "set:", 4) == 0 ||
                                             strncmp(md->name, "get:", 4) == 0);
            XrType *ret_type = md->return_type ? (XrType*)md->return_type : NULL;
            if (!ret_type && is_accessor && md->body) {
                ret_type = xa_infer_function_return_type(ctx, md->body);
            }
            if (!ret_type) {
                ret_type = xr_type_new_void(NULL);
            }
            if (!md->return_type && !md->is_constructor && !is_accessor && md->body) {
                if (xa_body_has_return_expr(md->body)) {
                    char msg[256];
                    snprintf(msg, sizeof(msg),
                        "Method '%s' returns a value but has no return type annotation",
                        md->name ? md->name : "?");
                    XrLocation loc = { .file = ctx->file_path, .line = method->line };
                    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                        XR_ERR_ANALYZE_MISSING_TYPE, msg, &loc);
                }
            }

            // Resolve CLASS("T") → TYPE_PARAM("T") for generic methods
            if (md->type_param_count > 0 && md->type_param_names) {
                for (int j = 0; j < md->param_count; j++) {
                    param_types[j] = resolve_class_to_type_param(NULL,
                        param_types[j], (const char**)md->type_param_names, md->type_param_count);
                }
                ret_type = resolve_class_to_type_param(NULL,
                    ret_type, (const char**)md->type_param_names, md->type_param_count);
            }

            XrType *method_type = xr_type_new_function(ctx->analyzer->isolate, param_types, md->param_count, ret_type, false);

            XaSymbolLinks *method_links = xa_analyzer_get_links(ctx->analyzer, method_sym);
            method_links->type = method_type;

            // Store parameter info for LSP
            xa_symbol_links_set_function_sig(method_links, param_types, param_names,
                                              md->param_count, ret_type);

            // Store generic type parameters for the method
            if (md->type_param_count > 0 && md->type_param_names) {
                const char **type_param_names = xr_malloc(sizeof(const char*) * md->type_param_count);
                XrType **type_param_constraints = xr_malloc(sizeof(XrType*) * md->type_param_count);

                for (int j = 0; j < md->type_param_count; j++) {
                    type_param_names[j] = md->type_param_names[j];
                    type_param_constraints[j] = NULL;  // TODO: support method type constraints
                }

                xa_symbol_links_set_type_params(method_links, type_param_names,
                                                 type_param_constraints, md->type_param_count);

                xr_free(type_param_names);
                xr_free(type_param_constraints);
            }

            xa_class_info_add_method(info, method_sym);

            // Record constructor info in class_info and validate super() rules
            if (md->is_constructor) {
                info->has_constructor = true;
                info->constructor_param_count = md->param_count;
                // Count required params (those without default values)
                int required = 0;
                for (int j = 0; j < md->param_count; j++) {
                    if (!md->default_values || !md->default_values[j])
                        required++;
                }
                info->constructor_required_params = required;
                validate_constructor_super_call(ctx, cls, md, method);
            }

            if (param_types) xr_free(param_types);
            if (param_names) xr_free(param_names);
        }
    }

    // Enter each method scope and add parameters + visit body for nested declarations.
    // This creates the function scopes that Pass 2 will reuse via ast_node matching,
    // ensuring method parameters are visible during type inference.
    for (int i = 0; i < cls->method_count; i++) {
        AstNode *method = cls->methods[i];
        if (!method || method->type != AST_METHOD_DECL) continue;
        MethodDeclNode *md = &method->as.method_decl;
        if (!md->body) continue;

        xa_analyzer_enter_scope(ctx->analyzer, XA_SCOPE_FUNCTION, method);

        // Look up method symbol to get resolved param types
        XaSymbol *msym = xa_scope_lookup_local(ctx->analyzer->current_scope->parent, md->name);
        XaSymbolLinks *mlinks = msym ? xa_analyzer_get_links(ctx->analyzer, msym) : NULL;

        for (int j = 0; j < md->param_count; j++) {
            const char *pname = md->parameters ? md->parameters[j] : NULL;
            if (!pname) continue;

            XaSymbol *param = xa_symbol_new(pname, XA_SYM_PARAMETER);
            param->location.line = method->line;
            if (md->param_passing_modes) {
                param->passing_mode = md->param_passing_modes[j];
            }
            xa_scope_add_symbol(ctx->analyzer->current_scope, param);

            XaSymbolLinks *plinks = xa_analyzer_get_links(ctx->analyzer, param);
            if (plinks) {
                plinks->type = (mlinks && mlinks->param_types && j < mlinks->param_count) ?
                    mlinks->param_types[j] : xr_type_new_unknown(NULL);
                plinks->is_definitely_assigned = true;
            }
        }

        // Visit body for nested declarations (variables, nested functions, etc.)
        xa_visit_collect(ctx, md->body);

        xa_analyzer_exit_scope(ctx->analyzer);
    }

    xa_analyzer_exit_scope(ctx->analyzer);
}

void xa_visit_collect_var_decl(XaInferContext *ctx, AstNode *node) {
    XR_DCHECK(ctx != NULL, "visit_collect_var_decl: NULL ctx");
    if (!node) return;

    VarDeclNode *var = &node->as.var_decl;

    XaSymbol *sym = xa_symbol_new(var->name, XA_SYM_VARIABLE);
    sym->location.line = node->line;
    sym->is_const = (node->type == AST_CONST_DECL);
    sym->is_shared = (var->storage_mode == 1);  // XR_STORAGE_SHARED

    xa_scope_add_symbol(ctx->analyzer->current_scope, sym);

    // Type will be inferred in pass 2
    // Keep NULL when no annotation (distinguishes "no annotation" from "annotated as any")
    XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, sym);
    links->declared_type = var->type_annotation;

    // Mark const types as immutable for concurrency safety
    if (sym->is_shared && sym->is_const && links->declared_type) {
        links->declared_type->is_const = true;
    }

    // Recurse into go { block } initializers to collect nested scopes.
    // go { ... } is parsed as go(anonymous_function_expr), whose body
    // needs Pass 1 scope collection for for-in variables, multi-value decls, etc.
    // Must mirror Pass 2 structure: function scope → block scope → statements
    // (xa_visit_function_expr enters function scope, then xa_visit_block_stmt
    //  enters block scope for the body).
    AstNode *init = var->initializer;
    if (init && init->type == AST_GO_EXPR) {
        AstNode *go_fn = init->as.go_expr.expr;
        if (go_fn && go_fn->type == AST_FUNCTION_EXPR) {
            FunctionDeclNode *fn = &go_fn->as.function_expr;
            xa_analyzer_enter_scope(ctx->analyzer, XA_SCOPE_FUNCTION, go_fn);
            if (fn->body && fn->body->type == AST_BLOCK) {
                xa_analyzer_enter_scope(ctx->analyzer, XA_SCOPE_BLOCK, fn->body);
                xa_visit_collect_statements_with_hoisting(ctx,
                    fn->body->as.block.statements, fn->body->as.block.count);
                xa_analyzer_exit_scope(ctx->analyzer);
            } else if (fn->body) {
                xa_visit_collect(ctx, fn->body);
            }
            xa_analyzer_exit_scope(ctx->analyzer);
        }
    }
}

/* ============================================================================
 * Pass 2: Type Inference - Entry Points
 * ============================================================================
 * xa_visit_infer()      - Main dispatch for expressions and statements
 * xa_visit_infer_expr() - Expression type inference
 * xa_visit_infer_stmt() - Statement type inference
 * ========================================================================== */

XrType *xa_visit_infer(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node) return xr_type_new_unknown(NULL);

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
        case AST_MAP_LITERAL:
        case AST_OBJECT_LITERAL:
        case AST_NEW_EXPR:
        case AST_TERNARY:
        case AST_FUNCTION_EXPR:
            return xa_visit_infer_expr(ctx, node);

        // Statements
        default:
            xa_visit_infer_stmt(ctx, node);
            return xr_type_new_void(NULL);
    }
}

XrType *xa_visit_infer_expr(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node) return xr_type_new_unknown(NULL);

    XrType *result;

    switch (node->type) {
        case AST_LITERAL_INT:
            result = xr_type_new_int(NULL); break;
        case AST_LITERAL_FLOAT:
            result = xr_type_new_float(NULL); break;
        case AST_LITERAL_STRING:
        case AST_TEMPLATE_STRING:
            result = xr_type_new_string(NULL); break;
        case AST_LITERAL_BIGINT:
            result = xr_type_new_bigint(ctx->analyzer->isolate); break;
        case AST_LITERAL_REGEX:
            result = xr_type_new_regex(ctx->analyzer->isolate); break;
        case AST_LITERAL_NULL:
            result = xr_type_new_null(NULL); break;
        case AST_LITERAL_TRUE:
        case AST_LITERAL_FALSE:
            result = xr_type_new_bool(NULL); break;
        case AST_VARIABLE:
            result = xa_visit_variable(ctx, node); break;
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
            result = xa_visit_binary(ctx, node); break;
        case AST_UNARY_NEG:
        case AST_UNARY_NOT:
        case AST_UNARY_BNOT:
            result = xa_visit_unary(ctx, node); break;
        case AST_CALL_EXPR:
            result = xa_visit_call(ctx, node); break;
        case AST_MEMBER_ACCESS:
            result = xa_visit_member_access(ctx, node); break;
        case AST_INDEX_GET:
            result = xa_visit_index_get(ctx, node); break;
        case AST_ARRAY_LITERAL:
            result = xa_visit_array_literal(ctx, node); break;
        case AST_MAP_LITERAL:
            result = xa_visit_map_literal(ctx, node); break;
        case AST_OBJECT_LITERAL:
            result = xa_visit_object_literal(ctx, node); break;
        case AST_NEW_EXPR:
            result = xa_visit_new_expr(ctx, node); break;
        case AST_STRUCT_LITERAL:
            result = xa_visit_struct_literal(ctx, node); break;
        case AST_TERNARY:
            result = xa_visit_ternary(ctx, node); break;
        case AST_FUNCTION_EXPR:
            result = xa_visit_function_expr(ctx, node); break;
        case AST_GROUPING:
            result = xa_visit_infer_expr(ctx, node->as.grouping); break;
        case AST_GO_EXPR:
            result = xa_visit_go_expr(ctx, node); break;
        case AST_AWAIT_EXPR:
            result = xa_visit_await_expr(ctx, node); break;
        case AST_MATCH_EXPR:
            result = xa_visit_match_expr(ctx, node); break;
        case AST_NULLISH_COALESCE:
            result = xa_visit_nullish_coalesce(ctx, node); break;
        case AST_OPTIONAL_CHAIN:
            result = xa_visit_optional_chain(ctx, node); break;
        case AST_FORCE_UNWRAP:
            result = xa_visit_force_unwrap(ctx, node); break;
        case AST_AS_EXPR:
            result = xa_visit_as_expr(ctx, node); break;
        case AST_IS_EXPR:
            result = xr_type_new_bool(NULL); break;
        case AST_RANGE:
            result = xr_type_new_named_instance(ctx->analyzer->isolate, "Range"); break;
        case AST_SET_LITERAL:
            result = xr_type_new_set(ctx->analyzer->isolate, xr_type_new_unknown(NULL)); break;
        case AST_CHANNEL_NEW:
            // Use expected_type if available (from type annotation: Channel<T>)
            if (ctx->expected_type && (ctx->expected_type->kind == XR_KIND_CHANNEL)) {
                result = ctx->expected_type;
            } else {
                result = xr_type_new_channel(ctx->analyzer->isolate, xr_type_new_unknown(NULL));
            }
            break;
        case AST_MOVE_EXPR:
            result = xa_visit_move_expr(ctx, node); break;
        default:
            result = xr_type_new_unknown(NULL); break;
    }

    // Propagate is_value_type for class/instance types created from type annotations
    // (parser doesn't know if a type name is struct vs class, so is_value_type defaults to false)
    if (result && !result->is_value_type &&
        (result->kind == XR_KIND_CLASS || result->kind == XR_KIND_INSTANCE) &&
        result->instance.class_name) {
        XaSymbol *_vs = xa_scope_lookup(ctx->analyzer->global_scope,
                                        result->instance.class_name);
        if (_vs && _vs->kind == XA_SYM_CLASS) {
            XaSymbolLinks *_vl = xa_analyzer_get_links(ctx->analyzer, _vs);
            if (_vl && _vl->type && _vl->type->is_value_type) {
                result->is_value_type = true;
            }
        }
    }

    // Cache inferred type on AST node for codegen phase
    node->compile_type = result;
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
    if (!body) return;

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
    if (!ctx || !node) return;

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
                        if (s->class_symbol) { class_sym = s->class_symbol; break; }
                        s = s->parent;
                    }
                }
                // Case 2: obj.field = expr — find class from instance type
                else if (XR_TYPE_IS_INSTANCE(obj_type) && obj_type->instance.class_name) {
                    class_sym = xa_scope_lookup(ctx->analyzer->global_scope,
                        obj_type->instance.class_name);
                }
                if (class_sym) {
                    XaSymbolLinks *class_links = xa_analyzer_get_links(ctx->analyzer, class_sym);
                    if (class_links && class_links->class_info) {
                        XaSymbol *field = xa_class_info_lookup_member(
                            class_links->class_info, ms->member);
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
            // Check in-parameter immutability: v.x = ... where v is 'in' param
            if (ms->object && ms->object->type == AST_VARIABLE) {
                const char *obj_name = ms->object->as.variable.name;
                XaSymbol *obj_sym = xa_scope_lookup(ctx->analyzer->current_scope, obj_name);
                if (obj_sym && obj_sym->kind == XA_SYM_PARAMETER &&
                    obj_sym->passing_mode == XR_PARAM_IN) {
                    XrLocation loc = { .file = ctx->file_path,
                                       .line = node->line, .column = node->column };
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
                    XrLocation loc = { .file = ctx->file_path,
                                       .line = inner->line, .column = inner->column };
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
                        XrLocation loc = { .file = ctx->file_path,
                                           .line = inner->line, .column = inner->column };
                        char msg[256];
                        snprintf(msg, sizeof(msg),
                            "Cannot modify field '%s' of 'in' parameter '%s' (readonly reference)",
                            ms->member, obj_name);
                        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                            XR_ERR_ANALYZE_CONST_ASSIGN, msg, &loc);
                    }
                }
            }
            if (inner && inner->type == AST_MEMBER_SET) {
                xa_visit_infer_stmt(ctx, inner);
            } else {
                xa_visit_infer_expr(ctx, inner);
            }
            break;
        }
        case AST_FUNCTION_DECL: {
            // Already collected in pass 1, infer body
            FunctionDeclNode *fn_decl = &node->as.function_decl;

            // P2-1: Save/restore return type state for nested functions
            XrType **saved_return_types = ctx->return_types;
            int saved_return_count = ctx->return_type_count;
            int saved_return_cap = ctx->return_type_capacity;
            ctx->return_types = NULL;
            ctx->return_type_count = 0;
            ctx->return_type_capacity = 0;

            xa_analyzer_enter_scope(ctx->analyzer, XA_SCOPE_FUNCTION, node);

            // Call-site inference feedback: update unannotated parameter types
            // with types inferred from call-sites during Pass 2.
            if (fn_decl->name) {
                XaSymbol *fn_sym = xa_scope_lookup(ctx->analyzer->current_scope->parent, fn_decl->name);
                XaSymbolLinks *fn_links = fn_sym ? xa_analyzer_get_links(ctx->analyzer, fn_sym) : NULL;
                if (fn_links && fn_links->inferred_param_types) {
                    for (int pi = 0; pi < fn_decl->param_count && pi < fn_links->inferred_param_count; pi++) {
                        XrType *inferred = fn_links->inferred_param_types[pi];
                        if (!inferred || XR_TYPE_IS_UNKNOWN(inferred)) continue;
                        XrParamNode *p = fn_decl->params[pi];
                        if (!p || !p->name || p->type) continue;  // skip annotated params
                        XaSymbol *param_sym = xa_scope_lookup_local(ctx->analyzer->current_scope, p->name);
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
                ctx->expected_return_type = (XrType *)fn_decl->return_type;
            } else {
                ctx->expected_return_type = xr_type_new_void(NULL);
            }

            // Unified body visitor: idempotent collect + direct traversal
            xa_visit_function_body_unified(ctx, fn_decl->body);

            // Re-check inferred_param_types after body analysis: recursive calls
            // inside the body may have widened param types (e.g. Json → Json?).
            if (fn_decl->name) {
                XaSymbol *fn_sym2 = xa_scope_lookup(ctx->analyzer->current_scope->parent, fn_decl->name);
                XaSymbolLinks *fn_links2 = fn_sym2 ? xa_analyzer_get_links(ctx->analyzer, fn_sym2) : NULL;
                if (fn_links2 && fn_links2->inferred_param_types) {
                    for (int pi = 0; pi < fn_decl->param_count && pi < fn_links2->inferred_param_count; pi++) {
                        XrType *inferred = fn_links2->inferred_param_types[pi];
                        if (!inferred || XR_TYPE_IS_UNKNOWN(inferred)) continue;
                        XrParamNode *p = fn_decl->params[pi];
                        if (!p || !p->name || p->type) continue;
                        XaSymbol *param_sym = xa_scope_lookup_local(ctx->analyzer->current_scope, p->name);
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
                if (inferred_ret && !XR_TYPE_IS_UNKNOWN(inferred_ret) && !XR_TYPE_IS_VOID(inferred_ret)) {
                    XaSymbol *fn_sym = xa_scope_lookup(ctx->analyzer->current_scope->parent, fn_decl->name);
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

            // Restore outer function's return type state
            if (ctx->return_types) xr_free(ctx->return_types);
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
            ClassDeclNode *cls = (node->type == AST_STRUCT_DECL)
                ? &node->as.struct_decl : &node->as.class_decl;
            xa_analyzer_enter_scope(ctx->analyzer, XA_SCOPE_CLASS, node);
            for (int i = 0; i < cls->method_count; i++) {
                if (cls->methods[i] && cls->methods[i]->as.method_decl.body) {
                    xa_analyzer_enter_scope(ctx->analyzer, XA_SCOPE_FUNCTION, cls->methods[i]);
                    // Save/restore expected_return_type so the enclosing
                    // function's type doesn't leak into method bodies.
                    XrType *saved_ret = ctx->expected_return_type;
                    MethodDeclNode *md = &cls->methods[i]->as.method_decl;
                    if (md->return_type) {
                        ctx->expected_return_type = (XrType *)md->return_type;
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
            if (tc->try_body) xa_visit_infer_stmt(ctx, tc->try_body);
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
                        err_links->type = xr_type_new_named_instance(ctx->analyzer->isolate, "Exception");
                        err_links->is_definitely_assigned = true;
                    }
                }
                xa_visit_infer_stmt(ctx, tc->catch_body);
                xa_analyzer_exit_scope(ctx->analyzer);
            }
            if (tc->finally_body) xa_visit_infer_stmt(ctx, tc->finally_body);
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
                    // Range literal: 0..N → int
                    coll_type = xr_type_new_int(NULL);  // placeholder, item is int
                } else if (fi->collection->type == AST_VARIABLE) {
                    // Check if variable is an enum
                    XaSymbol *coll_sym = xa_scope_lookup(ctx->analyzer->current_scope,
                                                          fi->collection->as.variable.name);
                    if (coll_sym && coll_sym->kind == XA_SYM_ENUM) {
                        is_enum_iter = true;
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
             *   Map<K,V>   → item: any (entry), or k: K, v: V
             *   Set<T>     → item: T
             *   Enum       → item: EnumValue
             *   Range      → item: int
             *   Channel<T> → item: T
             *   string     → item: string
             *   other      → item: any
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
                        item_type = coll_type->map.key_type ? coll_type->map.key_type : xr_type_new_unknown(NULL);
                        value_type = coll_type->map.value_type ? coll_type->map.value_type : xr_type_new_unknown(NULL);
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
                } else {
                    // Custom iterable: check iterator() -> next() protocol
                    XrType *elem = NULL;
                    if (xa_analyzer_is_iterable(ctx->analyzer, coll_type, &elem) && elem) {
                        item_type = elem;
                    }
                }
            }

            // Set item variable type
            if (fi->item_name) {
                XaSymbol *item_sym = xa_scope_lookup(ctx->analyzer->current_scope, fi->item_name);
                if (item_sym) {
                    XaSymbolLinks *item_links = xa_analyzer_get_links(ctx->analyzer, item_sym);
                    if (item_links) item_links->type = item_type;
                }
            }

            // Set value variable type (key-value mode)
            if (fi->is_keyvalue && fi->value_name) {
                XaSymbol *val_sym = xa_scope_lookup(ctx->analyzer->current_scope, fi->value_name);
                if (val_sym) {
                    XaSymbolLinks *val_links = xa_analyzer_get_links(ctx->analyzer, val_sym);
                    if (val_links) val_links->type = value_type;
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
            XrType *init_type = dd->initializer ? xa_visit_infer_expr(ctx, dd->initializer) : xr_type_new_unknown(NULL);

            // Set types on bound variables
            if (dd->pattern) {
                XrDestructurePattern *pat = dd->pattern;
                if (pat->type == PATTERN_ARRAY) {
                    XrType *elem_type = (init_type && XR_TYPE_IS_ARRAY(init_type) && init_type->container.element_type)
                        ? init_type->container.element_type : xr_type_new_unknown(NULL);
                    for (int i = 0; i < pat->as.array.element_count; i++) {
                        XrDestructurePattern *elem = pat->as.array.elements[i];
                        if (elem && elem->type == PATTERN_IDENTIFIER && elem->as.identifier.name) {
                            XaSymbol *sym = xa_scope_lookup(ctx->analyzer->current_scope, elem->as.identifier.name);
                            if (sym) {
                                XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, sym);
                                if (links) {
                                    links->type = elem_type;
                                    links->is_definitely_assigned = true;
                                }
                            }
                        }
                    }
                } else if (pat->type == PATTERN_OBJECT) {
                    for (int i = 0; i < pat->as.object.field_count; i++) {
                        XrDestructurePattern *vp = pat->as.object.patterns[i];
                        if (vp && vp->type == PATTERN_IDENTIFIER && vp->as.identifier.name) {
                            XaSymbol *sym = xa_scope_lookup(ctx->analyzer->current_scope, vp->as.identifier.name);
                            if (sym) {
                                XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, sym);
                                if (links) {
                                    // Try to infer field type from init_type
                                    XrType *field_type = NULL;
                                    const char *field_name = (pat->as.object.field_names && i < pat->as.object.field_count)
                                        ? pat->as.object.field_names[i] : vp->as.identifier.name;
                                    if (init_type && field_name) {
                                        field_type = xr_type_object_get_field(init_type, field_name);
                                    }
                                    links->type = field_type ? field_type : xr_type_new_unknown(NULL);
                                    links->is_definitely_assigned = true;
                                }
                            }
                        }
                    }
                }
            }
            break;
        }
        case AST_MULTI_VAR_DECL: {
            MultiVarDeclNode *mv = &node->as.multi_var_decl;
            for (int i = 0; i < mv->name_count; i++) {
                if (mv->names[i]) {
                    XaSymbol *sym = xa_scope_lookup(ctx->analyzer->current_scope, mv->names[i]);
                    if (sym) {
                        XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, sym);
                        if (links) {
                            XrType *val_type = (mv->values && i < mv->value_count && mv->values[i])
                                ? xa_visit_infer_expr(ctx, mv->values[i]) : xr_type_new_unknown(NULL);
                            links->type = val_type;
                            links->is_definitely_assigned = true;
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
        default:
            break;
    }
}
/* ============================================================================
 * Pass 1.5: Link Class Inheritance
 * ============================================================================
 * After Pass 1 collects all class symbols, this pass links inheritance chains
 * by resolving base class names to actual XrClassInfo pointers.
 * ========================================================================== */

// Build virtual method table for a class (inherits base vtable + own methods)
static void build_class_vtable(XaAnalyzer *analyzer, XrClassInfo *info) {
    if (!info || info->vtable) return;  // already built

    // First build base vtable if needed
    if (info->base) {
        build_class_vtable(analyzer, info->base);
    }

    // Determine vtable size: base methods + new methods
    int base_size = info->base ? info->base->vtable_size : 0;
    int max_size = base_size + info->method_count;
    if (max_size == 0) return;

    XaMethodSlot *vtable = xr_calloc(max_size, sizeof(XaMethodSlot));
    int vt_count = 0;

    // Copy base vtable entries (inherit)
    if (info->base && info->base->vtable) {
        for (int i = 0; i < info->base->vtable_size; i++) {
            vtable[i] = info->base->vtable[i];
            vtable[i].is_final = true;  // assume final until proven otherwise
            vt_count++;
        }
    }

    // Process own methods: override existing or add new
    for (int m = 0; m < info->method_count; m++) {
        XaSymbol *method = info->methods[m];
        if (!method || !method->name) continue;

        // Check if this overrides a base method
        bool found = false;
        for (int v = 0; v < vt_count; v++) {
            if (vtable[v].name && strcmp(vtable[v].name, method->name) == 0) {
                // Override: mark base method as overridden
                if (info->base && info->base->vtable) {
                    for (int bv = 0; bv < info->base->vtable_size; bv++) {
                        if (info->base->vtable[bv].name &&
                            strcmp(info->base->vtable[bv].name, method->name) == 0) {
                            info->base->vtable[bv].is_overridden = true;
                            info->base->vtable[bv].is_final = false;
                            break;
                        }
                    }
                }
                // Update slot to point to overriding method
                vtable[v].symbol = method;
                vtable[v].is_overridden = false;
                vtable[v].is_final = true;
                found = true;
                break;
            }
        }

        if (!found) {
            // New method, add to vtable
            vtable[vt_count].name = method->name;
            vtable[vt_count].symbol = method;
            vtable[vt_count].is_overridden = false;
            vtable[vt_count].is_final = true;
            vtable[vt_count].vtable_index = vt_count;
            vt_count++;
        }
    }

    // Assign vtable indices
    for (int i = 0; i < vt_count; i++) {
        vtable[i].vtable_index = i;
    }

    info->vtable = vtable;
    info->vtable_size = vt_count;
}

void xa_link_class_inheritance(XaAnalyzer *analyzer) {
    if (!analyzer || !analyzer->global_scope) return;

    // Get all symbols from global scope
    int count = 0;
    XaSymbol **symbols = xa_scope_get_all_symbols(analyzer->global_scope, &count);
    if (!symbols) return;

    // Pass 1: Link all class inheritance chains
    for (int i = 0; i < count; i++) {
        XaSymbol *sym = symbols[i];
        if (!sym || sym->kind != XA_SYM_CLASS) continue;

        XaSymbolLinks *links = xa_analyzer_get_links(analyzer, sym);
        if (!links || !links->class_info) continue;

        XrClassInfo *info = links->class_info;
        if (!info->base_name) continue;

        XaSymbol *base_sym = xa_scope_lookup(analyzer->global_scope, info->base_name);
        if (base_sym && base_sym->kind == XA_SYM_CLASS) {
            XaSymbolLinks *base_links = xa_analyzer_get_links(analyzer, base_sym);
            if (base_links && base_links->class_info) {
                info->base = base_links->class_info;
                base_links->class_info->has_subclass = true;
            }
        } else {
            info->base = NULL;
        }
    }

    // Pass 2: Build virtual method tables (after all inheritance is linked)
    for (int i = 0; i < count; i++) {
        XaSymbol *sym = symbols[i];
        if (!sym || sym->kind != XA_SYM_CLASS) continue;

        XaSymbolLinks *links = xa_analyzer_get_links(analyzer, sym);
        if (!links || !links->class_info) continue;

        build_class_vtable(analyzer, links->class_info);
    }

    // Pass 3: Mark methods as non-final if class has subclass
    // (A method is only truly final if no subclass exists)
    for (int i = 0; i < count; i++) {
        XaSymbol *sym = symbols[i];
        if (!sym || sym->kind != XA_SYM_CLASS) continue;
        XaSymbolLinks *links = xa_analyzer_get_links(analyzer, sym);
        if (!links || !links->class_info) continue;
        XrClassInfo *info = links->class_info;
        if (!info->vtable) continue;

        // If class has no subclass, all its methods are definitively final
        // (is_final = true is already default)
        // If class has subclass, methods not overridden are still final
        // (handled above during vtable build)
    }

    xr_free(symbols);
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
    if (!analyzer || !ast) return;

    XaInferContext *ctx = xa_infer_context_new(analyzer);
    if (!ctx) return;

    // Pass 1: Collect all symbols
    xa_visit_collect(ctx, ast);

    // Pass 1.5: Link class inheritance chains
    xa_link_class_inheritance(analyzer);

    // Pass 2: Infer types
    xa_visit_infer(ctx, ast);

    xa_infer_context_free(ctx);

#ifdef XR_ENABLE_JIT
    // Pass 3: Generate JIT/AOT metadata (type stability, func summaries, etc.)
    // This metadata is stored on analyzer->jit_metadata for the compiler to use.
    if (!analyzer->jit_metadata) {
        analyzer->jit_metadata = xa_jit_metadata_new();
    }
    xa_generate_jit_metadata(analyzer, ast, analyzer->jit_metadata);
#endif
}
