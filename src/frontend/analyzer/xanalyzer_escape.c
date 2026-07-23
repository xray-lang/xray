/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xanalyzer_escape.c - Coroutine sharing validation pass
 *
 * KEY CONCEPT:
 *   Walks the AST with a scope stack and rejects patterns that violate
 *   the explicit-sharing model. See xanalyzer_escape.h for the rule set.
 *
 * WHY THIS DESIGN:
 *   - Single-pass AST walk with a scope stack for name resolution
 *   - Pure diagnostic (no AST mutation, no auto-promote)
 *   - Function-local: captures outside the current function body are
 *     analyzed against the enclosing scope stack
 */

#include "xanalyzer_escape.h"
#include "xanalyzer.h"
#include "xanalyzer_capability.h"
#include "../../base/xchecks.h"
#include "../../base/xmalloc.h"
#include "../../runtime/xerror.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ========== Scope Stack for Name Resolution ========== */

// Initial capacities; both the per-scope variable table and the scope stack
// grow dynamically. Fixed caps previously (128 vars / 64 depth) silently
// dropped entries in large functions, which made go-capture checks miss
// variables past the limit (P1-9). Dynamic growth removes that blind spot.
#define EA_INITIAL_VARS 16
#define EA_INITIAL_SCOPES 16

typedef struct {
    const char *name;
    AstNode *decl_node;  // AST_VAR_DECL or AST_CONST_DECL node
    bool is_param;
    bool is_const_binding;
    XrParamMode param_passing_mode;
    // If this variable was initialized with a function-expression literal
    // (`var f = fn() {...}`), points to that FUNCTION_EXPR node. Lets
    // `go f()` apply the same capture enforcement as an inline `go fn(){}()`
    // instead of silently skipping the check.
    AstNode *bound_fn_expr;
} EaVarEntry;

typedef struct {
    EaVarEntry *vars;  // dynamic array, grows on demand
    int count;
    int capacity;
} EaScope;

typedef struct {
    EaScope *scopes;        // dynamic array, grows on demand
    int depth;              // current scope index (0 = top-level)
    int scopes_capacity;    // allocated length of `scopes`
    int skipped_scopes;     // pushes dropped under OOM; keeps push/pop balanced
    int func_boundary;      // scope depth at current function entry
    bool in_go_closure;     // true when inside a go-spawned closure body
    int go_scope_boundary;  // scope depth where go closure starts (captures below this are outer)
    bool in_fn_closure;     // true when inside a non-go function-expression body
    int fn_scope_boundary;  // scope depth where the function expression starts
    XaAnalyzer *analyzer;   // optional: for emitting diagnostics
    const char *file_path;  // source file path for diagnostic locations
} EaContext;

/*
 * Emit an analyzer diagnostic if an analyzer handle is attached.
 * Location is derived from the offending AST node (fallback: decl node).
 */
static void ea_emit_error(EaContext *ctx, AstNode *loc_node, int code, const char *fmt,
                          const char *name) {
    if (!ctx->analyzer || !loc_node)
        return;
    char msg[512];
    snprintf(msg, sizeof(msg), fmt, name, name, name);
    XrLocation loc = {
        .file = ctx->file_path,
        .line = loc_node->line,
        .column = loc_node->column,
    };
    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, code, msg, &loc);
}

static void ea_push_scope(EaContext *ctx) {
    if (ctx->depth + 1 >= ctx->scopes_capacity) {
        int new_cap = ctx->scopes_capacity ? ctx->scopes_capacity * 2 : EA_INITIAL_SCOPES;
        EaScope *grown = (EaScope *) xr_realloc(ctx->scopes, (size_t) new_cap * sizeof(EaScope));
        if (!grown) {
            // OOM: cannot grow the scope stack. Skip this push but remember it
            // so the matching pop stays balanced. Analysis degrades (variables
            // in this scope go unchecked) but remains memory-safe.
            ctx->skipped_scopes++;
            return;
        }
        // Zero the freshly added slots: vars=NULL, count=0, capacity=0.
        memset(grown + ctx->scopes_capacity, 0,
               (size_t) (new_cap - ctx->scopes_capacity) * sizeof(EaScope));
        ctx->scopes = grown;
        ctx->scopes_capacity = new_cap;
    }
    ctx->depth++;
    ctx->scopes[ctx->depth].count = 0;  // reuse any previously allocated vars buffer
}

static void ea_pop_scope(EaContext *ctx) {
    if (ctx->skipped_scopes > 0) {
        ctx->skipped_scopes--;
        return;
    }
    if (ctx->depth > 0)
        ctx->depth--;
}

static void ea_register_var_entry(EaContext *ctx, const char *name, AstNode *decl, bool is_param,
                                  bool is_const_binding, XrParamMode param_passing_mode) {
    if (ctx->depth >= ctx->scopes_capacity)
        return;  // defensive: only reachable if a push was OOM-skipped
    EaScope *scope = &ctx->scopes[ctx->depth];
    if (scope->count >= scope->capacity) {
        int new_cap = scope->capacity ? scope->capacity * 2 : EA_INITIAL_VARS;
        EaVarEntry *grown =
            (EaVarEntry *) xr_realloc(scope->vars, (size_t) new_cap * sizeof(EaVarEntry));
        if (!grown)
            return;  // OOM: skip registering this variable (memory-safe degrade)
        scope->vars = grown;
        scope->capacity = new_cap;
    }
    scope->vars[scope->count].name = name;
    scope->vars[scope->count].decl_node = decl;
    scope->vars[scope->count].is_param = is_param;
    scope->vars[scope->count].is_const_binding = is_const_binding;
    scope->vars[scope->count].param_passing_mode = param_passing_mode;
    scope->vars[scope->count].bound_fn_expr = NULL;
    scope->count++;
}

static void ea_register_var(EaContext *ctx, const char *name, AstNode *decl) {
    ea_register_var_entry(ctx, name, decl, false, false, XR_PARAM_READ);
}

static void ea_register_destructure_pattern(EaContext *ctx, XrDestructurePattern *pattern,
                                            AstNode *decl, bool is_const) {
    if (!pattern)
        return;
    switch (pattern->type) {
        case PATTERN_IDENTIFIER:
            if (pattern->as.identifier.name)
                ea_register_var_entry(ctx, pattern->as.identifier.name, decl, false, is_const,
                                      XR_PARAM_READ);
            break;
        case PATTERN_ARRAY:
        case PATTERN_TUPLE:
            for (int i = 0; i < pattern->as.array.element_count; i++)
                ea_register_destructure_pattern(ctx, pattern->as.array.elements[i], decl, is_const);
            break;
        case PATTERN_OBJECT:
            for (int i = 0; i < pattern->as.object.field_count; i++)
                ea_register_destructure_pattern(ctx, pattern->as.object.patterns[i], decl,
                                                is_const);
            break;
        case PATTERN_SKIP:
            break;
    }
}

// Register a variable that is bound to a function-expression literal, so a
// later `go var()` can enforce the same capture rules as an inline closure.
static void ea_register_var_with_fn(EaContext *ctx, const char *name, AstNode *decl,
                                    AstNode *bound_fn, bool is_const) {
    ea_register_var_entry(ctx, name, decl, false, is_const, XR_PARAM_READ);
    if (bound_fn) {
        EaScope *scope = &ctx->scopes[ctx->depth];
        if (scope->count > 0)
            scope->vars[scope->count - 1].bound_fn_expr = bound_fn;
    }
}

// Look up the function-expression a variable is bound to, searching outer
// scopes (a `go f()` typically references an `f` declared in an enclosing
// scope). Returns NULL if the variable is not found or not bound to a literal
// closure — in which case the runtime deep-copy fallback (fix B) is the
// safety net.
static AstNode *ea_lookup_bound_fn(EaContext *ctx, const char *name) {
    for (int d = ctx->depth; d >= 0; d--) {
        EaScope *scope = &ctx->scopes[d];
        for (int i = scope->count - 1; i >= 0; i--) {
            if (scope->vars[i].name && strcmp(scope->vars[i].name, name) == 0)
                return scope->vars[i].bound_fn_expr;
        }
    }
    return NULL;
}

static XrType *ea_entry_type(EaContext *ctx, const EaVarEntry *entry) {
    if (!ctx || !ctx->analyzer || !entry || !entry->decl_node ||
        (entry->decl_node->type != AST_VAR_DECL && entry->decl_node->type != AST_CONST_DECL))
        return NULL;
    uint32_t symbol_id = entry->decl_node->as.var_decl.symbol_id;
    XaSymbol *symbol =
        symbol_id ? xa_scope_lookup_by_id(ctx->analyzer->global_scope, symbol_id) : NULL;
    return symbol ? xa_analyzer_get_type(ctx->analyzer, symbol) : NULL;
}

/*
 * Check if a name exists in the current function scope (including params).
 * Used to distinguish local variables from captured outer variables.
 */
static bool ea_is_local_name(EaContext *ctx, const char *name) {
    for (int d = ctx->depth; d >= ctx->func_boundary; d--) {
        EaScope *scope = &ctx->scopes[d];
        for (int i = scope->count - 1; i >= 0; i--) {
            if (scope->vars[i].name && strcmp(scope->vars[i].name, name) == 0) {
                return true;
            }
        }
    }
    return false;
}

/*
 * Register function parameters in the current scope so they are
 * recognized as local names during capture analysis.
 */
static void ea_register_params(EaContext *ctx, FunctionDeclNode *fn) {
    for (int i = 0; i < fn->param_count; i++) {
        if (fn->params[i] && fn->params[i]->name) {
            ea_register_var_entry(ctx, fn->params[i]->name, NULL, true, false,
                                  fn->params[i]->passing_mode);
        }
    }
}

/*
 * Look up a variable captured by a go closure and enforce the explicit
 * sharing model. Ordinary mutable bindings are rejected even when the
 * closure only reads them: the rule cannot depend on scheduler topology or
 * a whole-program count of spawned coroutines.
 *
 *   - shared          -> allowed (user declared shared identity)
 *   - function parameter -> ERROR (must pass via explicit go argument)
 *   - plain var/const    -> ERROR (must pass via explicit go argument)
 */
static void ea_mark_capture_for_go(EaContext *ctx, AstNode *ref_node, const char *name) {
    for (int d = ctx->go_scope_boundary - 1; d >= 0; d--) {
        EaScope *scope = &ctx->scopes[d];
        for (int i = scope->count - 1; i >= 0; i--) {
            if (scope->vars[i].name && strcmp(scope->vars[i].name, name) == 0) {
                EaVarEntry *entry = &scope->vars[i];
                if (entry->is_param) {
                    XrParamMode mode = entry->param_passing_mode;
                    if (mode == XR_PARAM_READ || mode == XR_PARAM_REF) {
                        ea_emit_error(ctx, ref_node, XR_ERR_ANALYZE_CLOSURE_CAPTURE,
                                      "go closure cannot capture borrowed parameter '%s'\n"
                                      "hint: pass an owned value or copy(%s) through an explicit "
                                      "go argument",
                                      name);
                    } else {
                        ea_emit_error(ctx, ref_node, XR_ERR_ANALYZE_CLOSURE_CAPTURE,
                                      "go closure cannot capture parameter '%s'\n"
                                      "hint: pass copy(%s) or move %s through an explicit go "
                                      "argument",
                                      name);
                    }
                    return;
                }
                if (entry->is_const_binding) {
                    XrType *type = ea_entry_type(ctx, entry);
                    const uint32_t interior = XA_TYPE_CAP_INTERIOR_MUTABLE;
                    const uint32_t sync = XA_TYPE_CAP_INTERIOR_MUTABLE | XA_TYPE_CAP_SYNC_SHAREABLE;
                    if (xa_type_has_capabilities(type, interior) &&
                        !xa_type_has_capabilities(type, sync)) {
                        ea_emit_error(
                            ctx, ref_node, XR_ERR_ANALYZE_CLOSURE_CAPTURE,
                            "go closure cannot capture non-shareable interior-mutable const '%s'\n"
                            "hint: keep it execution-local, transfer one owner explicitly, or "
                            "route shared state through Channel/Atomic/Mutex",
                            name);
                    }
                    return;
                }
                ea_emit_error(ctx, ref_node, XR_ERR_ANALYZE_CLOSURE_CAPTURE,
                              "go closure cannot capture mutable variable '%s'\n"
                              "hint: route shared updates through Channel/Atomic/Mutex, or "
                              "transfer one owner with go worker(move %s)",
                              name);
                return;
            }
        }
    }
}

static void ea_mark_borrowed_capture_for_closure(EaContext *ctx, AstNode *ref_node,
                                                 const char *name) {
    for (int d = ctx->fn_scope_boundary - 1; d >= 0; d--) {
        EaScope *scope = &ctx->scopes[d];
        for (int i = scope->count - 1; i >= 0; i--) {
            EaVarEntry *entry = &scope->vars[i];
            if (!entry->name || strcmp(entry->name, name) != 0)
                continue;
            /* READ may be captured as a same-execution local alias; its
             * canonical parameter summary records that retain. REF remains a
             * non-escaping exclusive loan. */
            if (entry->is_param && entry->param_passing_mode == XR_PARAM_REF) {
                ea_emit_error(ctx, ref_node, XR_ERR_ANALYZE_CLOSURE_CAPTURE,
                              "closure cannot capture borrowed parameter '%s'\n"
                              "hint: capture an owned value or copy(%s) instead",
                              name);
            }
            return;
        }
    }
}

static void ea_mark_named_access(EaContext *ctx, AstNode *node, const char *name) {
    if ((!ctx->in_go_closure && !ctx->in_fn_closure) || !name || ea_is_local_name(ctx, name))
        return;
    if (ctx->in_go_closure)
        ea_mark_capture_for_go(ctx, node, name);
    else if (ctx->in_fn_closure)
        ea_mark_borrowed_capture_for_closure(ctx, node, name);
}

static void ea_mark_destructure_targets(EaContext *ctx, AstNode *node,
                                        XrDestructurePattern *pattern) {
    if (!pattern)
        return;
    switch (pattern->type) {
        case PATTERN_IDENTIFIER:
            ea_mark_named_access(ctx, node, pattern->as.identifier.name);
            break;
        case PATTERN_ARRAY:
        case PATTERN_TUPLE:
            for (int i = 0; i < pattern->as.array.element_count; i++)
                ea_mark_destructure_targets(ctx, node, pattern->as.array.elements[i]);
            break;
        case PATTERN_OBJECT:
            for (int i = 0; i < pattern->as.object.field_count; i++)
                ea_mark_destructure_targets(ctx, node, pattern->as.object.patterns[i]);
            break;
        case PATTERN_SKIP:
            break;
    }
}

/* ========== AST Walk ========== */

static void ea_walk(EaContext *ctx, AstNode *node);
static void ea_walk_statements(EaContext *ctx, AstNode **stmts, int count);

/*
 * Validate `move var` arguments seen by coroutine/channel boundaries:
 *   - shared    -> rejected by the type checker; already shared identity
 *   - plain var -> allowed by move strategy B; go call paths consume the
 *                      source at the boundary, channel paths keep their
 *                      existing send-side isolation
 *   - const/shared/thread-safe primitives -> rejected by xa_visit_move_expr
 */
static void ea_note_move_arg(EaContext *ctx, AstNode *ref_node, const char *var_name) {
    // Move strategy B allows mutable locals; this pass only
    // keeps the cross-go capture discipline. The type checker handles const
    // and thread-safe runtime primitives, and records use-after-move state.
    (void) ctx;
    (void) ref_node;
    (void) var_name;
}

/*
 * Check arguments of a go call or channel send boundary for move expressions.
 * For each 'move var', keep the move visible to the normal expression walk.
 */
static void ea_check_move_args(EaContext *ctx, AstNode **args, int arg_count) {
    for (int i = 0; i < arg_count; i++) {
        AstNode *arg = args[i];
        if (!arg)
            continue;
        if (arg->type == AST_MOVE_EXPR) {
            AstNode *inner = arg->as.move_expr.expr;
            if (inner && inner->type == AST_VARIABLE) {
                ea_note_move_arg(ctx, arg, inner->as.variable.name);
            }
        }
        // Also walk the argument (it may contain nested expressions)
        ea_walk(ctx, arg);
    }
}

/*
 * Historical shared move checks are no longer needed: shared is a stable
 * shared identity, not a movable owner slot.
 */
static void ea_check_go_shared_args(EaContext *ctx, AstNode **args, int arg_count) {
    (void) ctx;
    (void) args;
    (void) arg_count;
}

/*
 * Check if a call expression is a channel send boundary candidate. Precise
 * receiver typing is handled later by the main analyzer; this pass only keeps
 * move expressions visible to the AST walk.
 */
static bool ea_is_channel_send(AstNode *callee) {
    if (!callee || callee->type != AST_MEMBER_ACCESS)
        return false;
    const char *name = callee->as.member_access.name;
    return name && (strcmp(name, "send") == 0 || strcmp(name, "trySend") == 0 ||
                    strcmp(name, "sendTimeout") == 0);
}

static bool ea_is_sys_thread_spawn(AstNode *callee) {
    if (!callee || callee->type != AST_MEMBER_ACCESS)
        return false;
    MemberAccessNode *spawn = &callee->as.member_access;
    if (!spawn->name || strcmp(spawn->name, "spawn") != 0 || !spawn->object ||
        spawn->object->type != AST_MEMBER_ACCESS)
        return false;
    MemberAccessNode *thread = &spawn->object->as.member_access;
    if (!thread->name || strcmp(thread->name, "Thread") != 0 || !thread->object ||
        thread->object->type != AST_VARIABLE)
        return false;
    const char *module_name = thread->object->as.variable.name;
    return module_name && strcmp(module_name, "sys") == 0;
}

static AstNode *ea_thread_spawn_body_arg(CallExprNode *call) {
    if (!call)
        return NULL;
    if (call->arg_count == 1)
        return call->arguments[0];
    if (call->arg_count == 2)
        return call->arguments[1];
    return NULL;
}

/*
 * Walk a function body that executes on a NEW coroutine, with go-closure
 * capture tracking enabled. Covers both spawn forms:
 *   go fn() { body }()   (inline closure callee)
 *   go { body }          (bare block, parsed as GO_EXPR -> FUNCTION_EXPR)
 * Both must enforce the explicit-sharing model: capturing locals across the
 * coroutine boundary is a compile error unless shared / Channel.
 */
static void ea_walk_go_closure(EaContext *ctx, FunctionDeclNode *fn) {
    ea_push_scope(ctx);
    int old_boundary = ctx->func_boundary;
    bool old_in_go = ctx->in_go_closure;
    int old_go_boundary = ctx->go_scope_boundary;

    ctx->func_boundary = ctx->depth;
    ctx->in_go_closure = true;
    ctx->go_scope_boundary = ctx->depth;
    ea_register_params(ctx, fn);
    if (fn->body)
        ea_walk(ctx, fn->body);

    ctx->func_boundary = old_boundary;
    ctx->in_go_closure = old_in_go;
    ctx->go_scope_boundary = old_go_boundary;
    ea_pop_scope(ctx);
}

static void ea_walk_function_expr_closure(EaContext *ctx, FunctionDeclNode *fn) {
    ea_push_scope(ctx);
    int old_boundary = ctx->func_boundary;
    bool old_in_closure = ctx->in_fn_closure;
    int old_closure_boundary = ctx->fn_scope_boundary;

    ctx->func_boundary = ctx->depth;
    ctx->in_fn_closure = true;
    ctx->fn_scope_boundary = ctx->depth;
    ea_register_params(ctx, fn);
    if (fn->body)
        ea_walk(ctx, fn->body);

    ctx->func_boundary = old_boundary;
    ctx->in_fn_closure = old_in_closure;
    ctx->fn_scope_boundary = old_closure_boundary;
    ea_pop_scope(ctx);
}

static void ea_walk_go_like_spawn(EaContext *ctx, AstNode *expr) {
    if (expr && expr->type == AST_CALL_EXPR) {
        CallExprNode *call = &expr->as.call_expr;
        ea_check_move_args(ctx, call->arguments, call->arg_count);
        ea_check_go_shared_args(ctx, call->arguments, call->arg_count);

        AstNode *callee = call->callee;
        if (callee && callee->type == AST_FUNCTION_EXPR) {
            ea_walk_go_closure(ctx, &callee->as.function_decl);
        } else if (callee && callee->type == AST_VARIABLE) {
            AstNode *bound = ea_lookup_bound_fn(ctx, callee->as.variable.name);
            if (bound && bound->type == AST_FUNCTION_EXPR) {
                ea_walk_go_closure(ctx, &bound->as.function_decl);
            } else {
                ea_walk(ctx, callee);
            }
        } else {
            ea_walk(ctx, callee);
        }
    } else if (expr && expr->type == AST_FUNCTION_EXPR) {
        ea_walk_go_closure(ctx, &expr->as.function_decl);
    } else if (expr) {
        ea_walk(ctx, expr);
    }
}

static void ea_walk(EaContext *ctx, AstNode *node) {
    if (!node)
        return;

    switch (node->type) {
        // ---- Scope-creating nodes ----
        case AST_PROGRAM:
            ea_walk_statements(ctx, node->as.program.statements, node->as.program.count);
            break;

        case AST_BLOCK:
            ea_push_scope(ctx);
            ea_walk_statements(ctx, node->as.block.statements, node->as.block.count);
            ea_pop_scope(ctx);
            break;

        // ---- Variable declarations: register in current scope ----
        case AST_VAR_DECL:
        case AST_CONST_DECL: {
            AstNode *init = node->as.var_decl.initializer;
            // Track closure literals bound to a name so `go f()` can be checked.
            AstNode *bound_fn = (init && init->type == AST_FUNCTION_EXPR) ? init : NULL;
            ea_register_var_with_fn(ctx, node->as.var_decl.name, node, bound_fn,
                                    node->as.var_decl.is_const);
            if (init) {
                ea_walk(ctx, init);
            }
            break;
        }

        case AST_DESTRUCTURE_DECL:
            ea_register_destructure_pattern(ctx, node->as.destructure_decl.pattern, node,
                                            node->as.destructure_decl.is_const);
            ea_walk(ctx, node->as.destructure_decl.initializer);
            break;

        // ---- Function declarations: new function boundary ----
        case AST_FUNCTION_DECL: {
            FunctionDeclNode *fn = &node->as.function_decl;
            ea_push_scope(ctx);
            int old_boundary = ctx->func_boundary;
            ctx->func_boundary = ctx->depth;
            ea_register_params(ctx, fn);
            if (fn->body)
                ea_walk(ctx, fn->body);
            ctx->func_boundary = old_boundary;
            ea_pop_scope(ctx);
            break;
        }

        case AST_FUNCTION_EXPR:
            ea_walk_function_expr_closure(ctx, &node->as.function_expr);
            break;

        // ---- go expression: check arguments for move + closure captures ----
        case AST_GO_EXPR: {
            ea_walk_go_like_spawn(ctx, node->as.go_expr.expr);
            break;
        }

        // ---- Call expression: check for channel send boundary pattern ----
        case AST_CALL_EXPR: {
            CallExprNode *call = &node->as.call_expr;
            if (ea_is_sys_thread_spawn(call->callee)) {
                if (call->arg_count == 2)
                    ea_walk(ctx, call->arguments[0]);
                ea_walk_go_like_spawn(ctx, ea_thread_spawn_body_arg(call));
                break;
            }
            ea_walk(ctx, call->callee);
            if (ea_is_channel_send(call->callee)) {
                ea_check_move_args(ctx, call->arguments, call->arg_count);
            } else {
                for (int i = 0; i < call->arg_count; i++) {
                    ea_walk(ctx, call->arguments[i]);
                }
            }
            break;
        }

        // ---- Control flow ----
        case AST_IF_STMT:
            ea_walk(ctx, node->as.if_stmt.condition);
            ea_walk(ctx, node->as.if_stmt.then_branch);
            ea_walk(ctx, node->as.if_stmt.else_branch);
            break;

        case AST_WHILE_STMT:
            ea_walk(ctx, node->as.while_stmt.condition);
            ea_walk(ctx, node->as.while_stmt.body);
            break;

        case AST_FOR_STMT:
            ea_push_scope(ctx);
            ea_walk(ctx, node->as.for_stmt.initializer);
            ea_walk(ctx, node->as.for_stmt.condition);
            ea_walk(ctx, node->as.for_stmt.increment);
            ea_walk(ctx, node->as.for_stmt.body);
            ea_pop_scope(ctx);
            break;

        case AST_FOR_IN_STMT:
            ea_push_scope(ctx);
            // Register loop variable as local (not a VarDeclNode, use NULL decl)
            if (node->as.for_in_stmt.item_name) {
                ea_register_var(ctx, node->as.for_in_stmt.item_name, NULL);
            }
            if (node->as.for_in_stmt.is_keyvalue && node->as.for_in_stmt.value_name) {
                ea_register_var(ctx, node->as.for_in_stmt.value_name, NULL);
            }
            ea_walk(ctx, node->as.for_in_stmt.collection);
            ea_walk(ctx, node->as.for_in_stmt.body);
            ea_pop_scope(ctx);
            break;

        case AST_TRY_CATCH:
            ea_walk(ctx, node->as.try_catch.try_body);
            for (int ci = 0; ci < node->as.try_catch.catch_count; ci++) {
                XrCatchClause *cc = node->as.try_catch.catch_clauses[ci];
                if (cc && cc->body) {
                    ea_push_scope(ctx);
                    ea_walk(ctx, cc->body);
                    ea_pop_scope(ctx);
                }
            }
            break;

        case AST_RETURN_STMT:
            for (int i = 0; i < node->as.return_stmt.value_count; i++) {
                ea_walk(ctx, node->as.return_stmt.values[i]);
            }
            break;

        // ---- Binary expressions ----
        case AST_BINARY_ADD:
        case AST_BINARY_SUB:
        case AST_BINARY_MUL:
        case AST_BINARY_DIV:
        case AST_BINARY_MOD:
        case AST_BINARY_BAND:
        case AST_BINARY_BOR:
        case AST_BINARY_BXOR:
        case AST_BINARY_LSHIFT:
        case AST_BINARY_RSHIFT:
        case AST_BINARY_EQ:
        case AST_BINARY_NE:
        case AST_BINARY_LT:
        case AST_BINARY_LE:
        case AST_BINARY_GT:
        case AST_BINARY_GE:
        case AST_BINARY_AND:
        case AST_BINARY_OR:
        case AST_NULLISH_COALESCE:
            ea_walk(ctx, node->as.binary.left);
            ea_walk(ctx, node->as.binary.right);
            break;

        // ---- Unary expressions ----
        case AST_UNARY_NEG:
        case AST_UNARY_NOT:
        case AST_UNARY_BNOT:
        case AST_FORCE_UNWRAP:
            ea_walk(ctx, node->as.unary.operand);
            break;

        case AST_ASSIGNMENT:
            ea_mark_named_access(ctx, node, node->as.assignment.name);
            ea_walk(ctx, node->as.assignment.value);
            break;

        case AST_COMPOUND_ASSIGNMENT:
            if (node->as.compound_assignment.object)
                ea_walk(ctx, node->as.compound_assignment.object);
            else
                ea_mark_named_access(ctx, node, node->as.compound_assignment.name);
            ea_walk(ctx, node->as.compound_assignment.value);
            break;

        case AST_DESTRUCTURE_ASSIGN:
            ea_mark_destructure_targets(ctx, node, node->as.destructure_assign.pattern);
            ea_walk(ctx, node->as.destructure_assign.value);
            break;

        case AST_MEMBER_ACCESS:
            ea_walk(ctx, node->as.member_access.object);
            break;

        case AST_INDEX_GET:
            ea_walk(ctx, node->as.index_get.array);
            ea_walk(ctx, node->as.index_get.index);
            break;

        case AST_INDEX_SET:
            ea_walk(ctx, node->as.index_set.array);
            ea_walk(ctx, node->as.index_set.index);
            ea_walk(ctx, node->as.index_set.value);
            break;

        case AST_MEMBER_SET:
            ea_walk(ctx, node->as.member_set.object);
            ea_walk(ctx, node->as.member_set.value);
            break;

        case AST_ARRAY_LITERAL:
            if (node->as.array_literal.is_repeat) {
                ea_walk(ctx, node->as.array_literal.repeat_value);
                ea_walk(ctx, node->as.array_literal.repeat_count);
            } else {
                for (int i = 0; i < node->as.array_literal.count; i++) {
                    ea_walk(ctx, node->as.array_literal.elements[i]);
                }
            }
            break;

        case AST_OBJECT_LITERAL:
            for (int i = 0; i < node->as.object_literal.count; i++) {
                ea_walk(ctx, node->as.object_literal.values[i]);
            }
            break;

        case AST_MAP_LITERAL:
            for (int i = 0; i < node->as.map_literal.count; i++) {
                ea_walk(ctx, node->as.map_literal.keys[i]);
                ea_walk(ctx, node->as.map_literal.values[i]);
            }
            break;

        case AST_TERNARY:
            ea_walk(ctx, node->as.ternary.condition);
            ea_walk(ctx, node->as.ternary.true_expr);
            ea_walk(ctx, node->as.ternary.false_expr);
            break;

        case AST_MOVE_EXPR:
            ea_walk(ctx, node->as.move_expr.expr);
            break;

        case AST_COMPTIME_EXPR:
            ea_walk(ctx, node->as.comptime_expr.expr);
            break;

        case AST_UNSAFE_EXPR:
            ea_walk(ctx, node->as.unsafe_expr.operand);
            break;
        case AST_AWAIT_EXPR:
            ea_walk(ctx, node->as.await_expr.expr);
            ea_walk(ctx, node->as.await_expr.into);
            break;

        case AST_SCOPE_BLOCK:
            ea_walk(ctx, node->as.scope_block.body);
            break;

        case AST_SELECT_STMT:
            for (int i = 0; i < node->as.select_stmt.case_count; i++) {
                ea_walk(ctx, node->as.select_stmt.cases[i]);
            }
            break;

        case AST_SELECT_CASE:
            ea_walk(ctx, node->as.select_case.channel);
            ea_walk(ctx, node->as.select_case.value);
            ea_walk(ctx, node->as.select_case.body);
            break;

        case AST_DEFER_STMT:
            ea_walk(ctx, node->as.defer_stmt.expr);
            break;

        case AST_MATCH_EXPR:
            ea_walk(ctx, node->as.match_expr.expr);
            for (int i = 0; i < node->as.match_expr.arm_count; i++) {
                ea_walk(ctx, node->as.match_expr.arms[i]);
            }
            break;

        case AST_MATCH_ARM:
            ea_walk(ctx, node->as.match_arm.pattern);
            ea_walk(ctx, node->as.match_arm.guard);
            ea_walk(ctx, node->as.match_arm.body);
            break;

        case AST_TEMPLATE_STRING:
            for (int i = 0; i < node->as.template_str.part_count; i++) {
                ea_walk(ctx, node->as.template_str.parts[i]);
            }
            break;

        case AST_THROW_STMT:
            ea_walk(ctx, node->as.throw_stmt.expression);
            break;

        case AST_PRINT_STMT:
            for (int i = 0; i < node->as.print_stmt.expr_count; i++) {
                ea_walk(ctx, node->as.print_stmt.exprs[i]);
            }
            break;

        case AST_EXPR_STMT:
            ea_walk(ctx, node->as.expr_stmt);
            break;

        case AST_SLICE_EXPR:
            ea_walk(ctx, node->as.slice_expr.source);
            ea_walk(ctx, node->as.slice_expr.start);
            ea_walk(ctx, node->as.slice_expr.end);
            break;

        case AST_GROUPING:
            ea_walk(ctx, node->as.grouping);
            break;

        case AST_IS_EXPR:
            ea_walk(ctx, node->as.is_expr.expr);
            break;

        case AST_AS_EXPR:
            ea_walk(ctx, node->as.as_expr.expr);
            break;

        case AST_CLASS_DECL:
        case AST_STRUCT_DECL:
        case AST_UNION_DECL: {
            ClassDeclNode *cls = (node->type == AST_CLASS_DECL)    ? &node->as.class_decl
                                 : (node->type == AST_STRUCT_DECL) ? &node->as.struct_decl
                                                                   : &node->as.union_decl;
            for (int i = 0; i < cls->method_count; i++) {
                ea_walk(ctx, cls->methods[i]);
            }
            break;
        }
        case AST_GLOBAL_ASM:
            break;

        case AST_METHOD_DECL: {
            MethodDeclNode *m = &node->as.method_decl;
            if (m->body) {
                ea_push_scope(ctx);
                int old_boundary = ctx->func_boundary;
                ctx->func_boundary = ctx->depth;
                // Register method params from the declaration ParamContract nodes.
                if (m->params) {
                    for (int i = 0; i < m->param_count; i++) {
                        if (m->params[i] && m->params[i]->name)
                            ea_register_var_entry(ctx, m->params[i]->name, NULL, true, false,
                                                  m->params[i]->passing_mode);
                    }
                }
                ea_walk(ctx, m->body);
                ctx->func_boundary = old_boundary;
                ea_pop_scope(ctx);
            }
            break;
        }

        // ---- Variable reference: check for go-closure captures ----
        case AST_VARIABLE: {
            if (ctx->in_go_closure) {
                const char *name = node->as.variable.name;
                if (name && !ea_is_local_name(ctx, name)) {
                    ea_mark_capture_for_go(ctx, node, name);
                }
            } else if (ctx->in_fn_closure) {
                const char *name = node->as.variable.name;
                if (name && !ea_is_local_name(ctx, name)) {
                    ea_mark_borrowed_capture_for_closure(ctx, node, name);
                }
            }
            break;
        }

        // ---- Leaf nodes ----
        case AST_LITERAL_INT:
        case AST_LITERAL_FLOAT:
        case AST_LITERAL_BIGINT:
        case AST_LITERAL_STRING:
        case AST_FIXED_BYTES_LITERAL:
        case AST_LITERAL_RUNE:
        case AST_LITERAL_NULL:
        case AST_LITERAL_TRUE:
        case AST_LITERAL_FALSE:
        case AST_BREAK_STMT:
        case AST_CONTINUE_STMT:
        case AST_THIS_EXPR:
        case AST_CANCELLED_EXPR:
        case AST_CHANNEL_NEW:
            break;
        case AST_INC:
            ea_mark_named_access(ctx, node, node->as.inc.name);
            break;
        case AST_DEC:
            ea_mark_named_access(ctx, node, node->as.dec.name);
            break;
        case AST_YIELD_STMT:
            ea_walk(ctx, node->as.yield_stmt.value);
            break;

        default:
            break;
    }
}

static void ea_walk_statements(EaContext *ctx, AstNode **stmts, int count) {
    for (int i = 0; i < count; i++) {
        ea_walk(ctx, stmts[i]);
    }
}

/* ========== Public API ========== */

void xa_escape_analyze(AstNode *ast, XaAnalyzer *analyzer) {
    if (!ast)
        return;

    EaContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.scopes = (EaScope *) xr_calloc(EA_INITIAL_SCOPES, sizeof(EaScope));
    if (!ctx.scopes)
        return;  // OOM: cannot run the pass
    ctx.scopes_capacity = EA_INITIAL_SCOPES;
    ctx.analyzer = analyzer;

    ea_walk(&ctx, ast);

    // Free every scope's variable buffer (buffers persist across push/pop for
    // reuse, so walk the whole allocated range, not just [0, depth]).
    for (int i = 0; i < ctx.scopes_capacity; i++)
        xr_free(ctx.scopes[i].vars);
    xr_free(ctx.scopes);
}
