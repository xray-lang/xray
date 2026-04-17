/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xanalyzer_jit.c - JIT/AOT metadata generation (Pass 3)
 *
 * KEY CONCEPT:
 *   Walks symbol table and AST after type inference to collect
 *   optimization metadata for JIT/AOT compilers.
 */

#include "xanalyzer_jit.h"
#include "../../base/xchecks.h"
#include "xanalyzer_visitor.h"
#include "xanalyzer_infer.h"
#include "../codegen/xoptimize.h"
#include "../parser/xast.h"
#include "../../base/xmalloc.h"
#include <string.h>
#include <stdio.h>

/* ========== Metadata Container ========== */

XaJitMetadata *xa_jit_metadata_new(void) {
    XaJitMetadata *meta = xr_calloc(1, sizeof(XaJitMetadata));
    return meta;
}

void xa_jit_metadata_free(XaJitMetadata *meta) {
    if (!meta) return;
    xr_free(meta->func_summaries);
    xr_free(meta->var_hints);
    xr_free(meta);
}

/* ========== Internal Helpers ========== */

static void add_func_summary(XaJitMetadata *meta, XaFuncSummary *summary) {
    XR_DCHECK(meta != NULL, "add_func_summary: NULL meta");
    XR_DCHECK(summary != NULL, "add_func_summary: NULL summary");
    if (meta->func_count >= meta->func_capacity) {
        int new_cap = meta->func_capacity ? meta->func_capacity * 2 : 16;
        XR_REALLOC_OR_ABORT(meta->func_summaries,
                            (size_t)new_cap * sizeof(XaFuncSummary),
                            "analyzer_jit func_summaries grow");
        meta->func_capacity = new_cap;
    }
    meta->func_summaries[meta->func_count++] = *summary;
}

static void add_var_hint(XaJitMetadata *meta, XaVarHint *hint) {
    XR_DCHECK(meta != NULL, "add_var_hint: NULL meta");
    XR_DCHECK(hint != NULL, "add_var_hint: NULL hint");
    if (meta->var_count >= meta->var_capacity) {
        int new_cap = meta->var_capacity ? meta->var_capacity * 2 : 32;
        XR_REALLOC_OR_ABORT(meta->var_hints,
                            (size_t)new_cap * sizeof(XaVarHint),
                            "analyzer_jit var_hints grow");
        meta->var_capacity = new_cap;
    }
    meta->var_hints[meta->var_count++] = *hint;
}

/* ========== Statement Counter ========== */

static int count_statements(AstNode *node) {
    if (!node) return 0;
    switch (node->type) {
        case AST_PROGRAM:
            return node->as.program.count;
        case AST_BLOCK:
            return node->as.block.count;
        default:
            return 1;
    }
}

/* ========== Loop Depth Calculator ========== */

static int calc_max_loop_depth(AstNode *node, int current_depth) {
    if (!node) return current_depth;
    int max_depth = current_depth;

    switch (node->type) {
        case AST_WHILE_STMT: {
            int body_depth = calc_max_loop_depth(node->as.while_stmt.body, current_depth + 1);
            if (body_depth > max_depth) max_depth = body_depth;
            break;
        }
        case AST_FOR_STMT: {
            int body_depth = calc_max_loop_depth(node->as.for_stmt.body, current_depth + 1);
            if (body_depth > max_depth) max_depth = body_depth;
            break;
        }
        case AST_FOR_IN_STMT: {
            int body_depth = calc_max_loop_depth(node->as.for_in_stmt.body, current_depth + 1);
            if (body_depth > max_depth) max_depth = body_depth;
            break;
        }
        case AST_BLOCK: {
            for (int i = 0; i < node->as.block.count; i++) {
                int d = calc_max_loop_depth(node->as.block.statements[i], current_depth);
                if (d > max_depth) max_depth = d;
            }
            break;
        }
        case AST_IF_STMT: {
            int d = calc_max_loop_depth(node->as.if_stmt.then_branch, current_depth);
            if (d > max_depth) max_depth = d;
            d = calc_max_loop_depth(node->as.if_stmt.else_branch, current_depth);
            if (d > max_depth) max_depth = d;
            break;
        }
        default:
            break;
    }
    return max_depth;
}

/* ========== Function Flags Detection ========== */

// Generic statement-level AST search with predicate
typedef bool (*BodyPredFn)(AstNode *node, void *ctx);

static bool body_search(AstNode *node, BodyPredFn pred, void *ctx) {
    if (!node) return false;
    if (pred(node, ctx)) return true;

    switch (node->type) {
        case AST_BLOCK:
            for (int i = 0; i < node->as.block.count; i++)
                if (body_search(node->as.block.statements[i], pred, ctx)) return true;
            return false;
        case AST_IF_STMT:
            return body_search(node->as.if_stmt.then_branch, pred, ctx) ||
                   body_search(node->as.if_stmt.else_branch, pred, ctx);
        case AST_WHILE_STMT:
            return body_search(node->as.while_stmt.body, pred, ctx);
        case AST_FOR_STMT:
            return body_search(node->as.for_stmt.body, pred, ctx);
        case AST_FOR_IN_STMT:
            return body_search(node->as.for_in_stmt.body, pred, ctx);
        case AST_RETURN_STMT:
            for (int i = 0; i < node->as.return_stmt.value_count; i++)
                if (body_search(node->as.return_stmt.values[i], pred, ctx)) return true;
            return false;
        case AST_EXPR_STMT:
            return body_search(node->as.expr_stmt, pred, ctx);
        default:
            return false;
    }
}

static bool pred_is_call(AstNode *node, void *ctx) {
    (void)ctx;
    return node->type == AST_CALL_EXPR;
}

static bool pred_is_coro_op(AstNode *node, void *ctx) {
    (void)ctx;
    return node->type == AST_GO_EXPR || node->type == AST_AWAIT_EXPR ||
           node->type == AST_AWAIT_ALL_EXPR || node->type == AST_AWAIT_ANY_EXPR;
}

static bool pred_is_self_call(AstNode *node, void *ctx) {
    const char *name = (const char *)ctx;
    return node->type == AST_CALL_EXPR && node->as.call_expr.callee &&
           node->as.call_expr.callee->type == AST_VARIABLE &&
           node->as.call_expr.callee->as.variable.name &&
           strcmp(node->as.call_expr.callee->as.variable.name, name) == 0;
}

// Count local variables in function body
static int count_locals(AstNode *node) {
    if (!node) return 0;
    int count = 0;

    switch (node->type) {
        case AST_VAR_DECL:
            return 1;
        case AST_BLOCK:
            for (int i = 0; i < node->as.block.count; i++)
                count += count_locals(node->as.block.statements[i]);
            return count;
        case AST_IF_STMT:
            count += count_locals(node->as.if_stmt.then_branch);
            count += count_locals(node->as.if_stmt.else_branch);
            return count;
        case AST_WHILE_STMT:
            return count_locals(node->as.while_stmt.body);
        case AST_FOR_STMT:
            return 1 + count_locals(node->as.for_stmt.body);
        case AST_FOR_IN_STMT:
            return 1 + count_locals(node->as.for_in_stmt.body);
        default:
            return 0;
    }
}

/* ========== Closure Detection ========== */

#define MAX_LOCAL_NAMES 256

typedef struct {
    const char *names[MAX_LOCAL_NAMES];
    int count;
} LocalNameSet;

static void local_set_add(LocalNameSet *s, const char *name) {
    if (name && s->count < MAX_LOCAL_NAMES)
        s->names[s->count++] = name;
}

static bool local_set_contains(LocalNameSet *s, const char *name) {
    for (int i = 0; i < s->count; i++) {
        if (strcmp(s->names[i], name) == 0) return true;
    }
    return false;
}

// Collect variable names declared inside a function body (stop at nested functions)
static void collect_func_locals(AstNode *node, LocalNameSet *set) {
    if (!node || set->count >= MAX_LOCAL_NAMES) return;
    switch (node->type) {
        case AST_VAR_DECL:
        case AST_CONST_DECL:
            local_set_add(set, node->as.var_decl.name);
            break;
        case AST_BLOCK:
            for (int i = 0; i < node->as.block.count; i++)
                collect_func_locals(node->as.block.statements[i], set);
            break;
        case AST_IF_STMT:
            collect_func_locals(node->as.if_stmt.then_branch, set);
            collect_func_locals(node->as.if_stmt.else_branch, set);
            break;
        case AST_WHILE_STMT:
            collect_func_locals(node->as.while_stmt.body, set);
            break;
        case AST_FOR_STMT:
            collect_func_locals(node->as.for_stmt.initializer, set);
            collect_func_locals(node->as.for_stmt.body, set);
            break;
        case AST_FOR_IN_STMT:
            local_set_add(set, node->as.for_in_stmt.item_name);
            if (node->as.for_in_stmt.is_keyvalue)
                local_set_add(set, node->as.for_in_stmt.value_name);
            collect_func_locals(node->as.for_in_stmt.body, set);
            break;
        case AST_TRY_CATCH:
            collect_func_locals(node->as.try_catch.try_body, set);
            local_set_add(set, node->as.try_catch.catch_var);
            collect_func_locals(node->as.try_catch.catch_body, set);
            collect_func_locals(node->as.try_catch.finally_body, set);
            break;
        case AST_FUNCTION_DECL:
        case AST_FUNCTION_EXPR:
            break;
        default:
            break;
    }
}

// Walk AST for variable references that are captured from outer scopes
// Returns true on first captured variable found
static bool has_captured_var(AstNode *node, LocalNameSet *locals, XaScope *global) {
    if (!node) return false;

    if (node->type == AST_VARIABLE) {
        const char *name = node->as.variable.name;
        if (!name) return false;
        if (local_set_contains(locals, name)) return false;
        // If defined in global scope, not a capture
        if (xa_scope_lookup(global, name)) return false;
        // Referenced name is neither local nor global → captured upvalue
        return true;
    }

    // Stop at nested function boundaries
    if (node->type == AST_FUNCTION_DECL || node->type == AST_FUNCTION_EXPR)
        return false;

    switch (node->type) {
        case AST_BLOCK:
            for (int i = 0; i < node->as.block.count; i++)
                if (has_captured_var(node->as.block.statements[i], locals, global)) return true;
            return false;
        case AST_IF_STMT:
            return has_captured_var(node->as.if_stmt.condition, locals, global) ||
                   has_captured_var(node->as.if_stmt.then_branch, locals, global) ||
                   has_captured_var(node->as.if_stmt.else_branch, locals, global);
        case AST_WHILE_STMT:
            return has_captured_var(node->as.while_stmt.condition, locals, global) ||
                   has_captured_var(node->as.while_stmt.body, locals, global);
        case AST_FOR_STMT:
            return has_captured_var(node->as.for_stmt.initializer, locals, global) ||
                   has_captured_var(node->as.for_stmt.condition, locals, global) ||
                   has_captured_var(node->as.for_stmt.increment, locals, global) ||
                   has_captured_var(node->as.for_stmt.body, locals, global);
        case AST_FOR_IN_STMT:
            return has_captured_var(node->as.for_in_stmt.collection, locals, global) ||
                   has_captured_var(node->as.for_in_stmt.body, locals, global);
        case AST_RETURN_STMT:
            for (int i = 0; i < node->as.return_stmt.value_count; i++)
                if (has_captured_var(node->as.return_stmt.values[i], locals, global)) return true;
            return false;
        case AST_EXPR_STMT:
            return has_captured_var(node->as.expr_stmt, locals, global);
        case AST_PRINT_STMT:
            for (int i = 0; i < node->as.print_stmt.expr_count; i++)
                if (has_captured_var(node->as.print_stmt.exprs[i], locals, global)) return true;
            return false;
        case AST_VAR_DECL:
        case AST_CONST_DECL:
            return has_captured_var(node->as.var_decl.initializer, locals, global);
        case AST_ASSIGNMENT: {
            const char *aname = node->as.assignment.name;
            if (aname && !local_set_contains(locals, aname) &&
                !xa_scope_lookup(global, aname))
                return true;
            return has_captured_var(node->as.assignment.value, locals, global);
        }
        case AST_CALL_EXPR:
            if (has_captured_var(node->as.call_expr.callee, locals, global)) return true;
            for (int i = 0; i < node->as.call_expr.arg_count; i++)
                if (has_captured_var(node->as.call_expr.arguments[i], locals, global)) return true;
            return false;
        case AST_MEMBER_ACCESS:
            return has_captured_var(node->as.member_access.object, locals, global);
        case AST_INDEX_GET:
            return has_captured_var(node->as.index_get.array, locals, global) ||
                   has_captured_var(node->as.index_get.index, locals, global);
        case AST_INDEX_SET:
            return has_captured_var(node->as.index_set.array, locals, global) ||
                   has_captured_var(node->as.index_set.index, locals, global) ||
                   has_captured_var(node->as.index_set.value, locals, global);
        case AST_BINARY_ADD: case AST_BINARY_SUB: case AST_BINARY_MUL:
        case AST_BINARY_DIV: case AST_BINARY_MOD: case AST_BINARY_EQ:
        case AST_BINARY_NE:  case AST_BINARY_LT:  case AST_BINARY_LE:
        case AST_BINARY_GT:  case AST_BINARY_GE:  case AST_BINARY_AND:
        case AST_BINARY_OR:  case AST_BINARY_BAND: case AST_BINARY_BOR:
        case AST_BINARY_BXOR: case AST_BINARY_LSHIFT: case AST_BINARY_RSHIFT:
            return has_captured_var(node->as.binary.left, locals, global) ||
                   has_captured_var(node->as.binary.right, locals, global);
        case AST_UNARY_NEG: case AST_UNARY_NOT: case AST_UNARY_BNOT:
            return has_captured_var(node->as.unary.operand, locals, global);
        case AST_TERNARY:
            return has_captured_var(node->as.ternary.condition, locals, global) ||
                   has_captured_var(node->as.ternary.true_expr, locals, global) ||
                   has_captured_var(node->as.ternary.false_expr, locals, global);
        case AST_ARRAY_LITERAL:
            for (int i = 0; i < node->as.array_literal.count; i++)
                if (has_captured_var(node->as.array_literal.elements[i], locals, global)) return true;
            return false;
        case AST_MAP_LITERAL:
            for (int i = 0; i < node->as.map_literal.count; i++) {
                if (has_captured_var(node->as.map_literal.keys[i], locals, global)) return true;
                if (has_captured_var(node->as.map_literal.values[i], locals, global)) return true;
            }
            return false;
        case AST_TRY_CATCH:
            return has_captured_var(node->as.try_catch.try_body, locals, global) ||
                   has_captured_var(node->as.try_catch.catch_body, locals, global) ||
                   has_captured_var(node->as.try_catch.finally_body, locals, global);
        case AST_THROW_STMT:
            return has_captured_var(node->as.throw_stmt.expression, locals, global);
        case AST_GO_EXPR:
            return has_captured_var(node->as.go_expr.expr, locals, global);
        case AST_AWAIT_EXPR:
            return has_captured_var(node->as.await_expr.expr, locals, global);
        default:
            return false;
    }
}

// Check if a function captures variables from enclosing scopes
static bool func_is_closure(XaAnalyzer *analyzer, FunctionDeclNode *fn) {
    LocalNameSet locals = {.count = 0};

    // Add parameter names
    for (int i = 0; i < fn->param_count; i++) {
        if (fn->params[i])
            local_set_add(&locals, fn->params[i]->name);
    }

    // Collect local variable declarations from body
    collect_func_locals(fn->body, &locals);

    // Walk body looking for captured variable references
    return has_captured_var(fn->body, &locals, analyzer->global_scope);
}

/* ========== Function Summary Generation ========== */

static void generate_func_summary(XaAnalyzer *analyzer, XaSymbol *sym,
                                   AstNode *func_node, XaJitMetadata *out) {
    XR_DCHECK(analyzer != NULL, "generate_func_summary: NULL analyzer");
    XR_DCHECK(out != NULL, "generate_func_summary: NULL out");
    if (!sym || !func_node) return;

    XaSymbolLinks *links = xa_analyzer_get_links(analyzer, sym);
    if (!links) return;

    FunctionDeclNode *fn = (func_node->type == AST_FUNCTION_DECL)
        ? &func_node->as.function_decl
        : &func_node->as.function_expr;

    XaFuncSummary summary = {0};
    summary.symbol_id = sym->id;
    summary.name = sym->name;
    summary.signature = links->type;
    summary.actual_return = links->return_type;

    // Compute flags
    uint32_t flags = 0;

    // Variadic
    for (int i = 0; i < fn->param_count; i++) {
        if (fn->params[i] && fn->params[i]->is_rest) {
            flags |= XA_FUNC_VARIADIC;
            break;
        }
    }

    // Leaf function (no user calls)
    if (!body_search(fn->body, pred_is_call, NULL))
        flags |= XA_FUNC_LEAF;

    // Recursive
    if (sym->name && body_search(fn->body, pred_is_self_call, (void*)sym->name))
        flags |= XA_FUNC_RECURSIVE;

    // Coroutine
    if (body_search(fn->body, pred_is_coro_op, NULL))
        flags |= XA_FUNC_COROUTINE;

    // Closure (actually captures variables from enclosing scope)
    if (fn->body && sym->scope && sym->scope->kind != XA_SCOPE_GLOBAL &&
        func_is_closure(analyzer, fn))
        flags |= XA_FUNC_CLOSURE;

    // Small function (inline candidate)
    int stmts = count_statements(fn->body);
    if (stmts <= 5)
        flags |= XA_FUNC_SMALL;

    summary.flags = flags;
    summary.loop_depth = calc_max_loop_depth(fn->body, 0);
    summary.stmt_count = stmts;
    summary.local_count = fn->param_count + count_locals(fn->body);

    // Call count from reference tracking
    summary.call_count = links->ref_count;

    // Hot candidate: called in loop or high call count
    if (summary.call_count > 3 || summary.loop_depth > 0)
        flags |= XA_FUNC_HOT_CANDIDATE;
    summary.flags = flags;

    add_func_summary(out, &summary);
}

/* ========== Variable Hint Generation ========== */

static void generate_var_hint(XaAnalyzer *analyzer, XaSymbol *sym, XaJitMetadata *out) {
    if (!sym) return;

    XaSymbolLinks *links = xa_analyzer_get_links(analyzer, sym);
    if (!links || !links->type) return;

    XaVarHint hint = {0};
    hint.symbol_id = sym->id;
    hint.opt_hint = (uint8_t)xr_opt_get_hint(links->type);
    hint.certainty = (uint8_t)links->jit_certainty;
    hint.stability = (uint8_t)links->type_stability;

    uint8_t flags = 0;
    if (links->type->is_nullable) flags |= XA_VAR_FLAG_NULLABLE;
    if (sym->is_const) flags |= XA_VAR_FLAG_CONST;
    if (links->is_loop_variable) flags |= XA_VAR_FLAG_LOOP_VAR;
    if (links->is_const_foldable) flags |= XA_VAR_FLAG_CONST_FOLDABLE;
    if (sym->is_shared) flags |= XA_VAR_FLAG_SHARED;
    hint.flags = flags;

    add_var_hint(out, &hint);
}

/* ========== Pass 3: Walk AST and Collect Metadata ========== */

static void pass3_walk(XaAnalyzer *analyzer, AstNode *node, XaJitMetadata *out) {
    if (!node) return;

    switch (node->type) {
        case AST_PROGRAM: {
            for (int i = 0; i < node->as.program.count; i++)
                pass3_walk(analyzer, node->as.program.statements[i], out);
            break;
        }

        case AST_FUNCTION_DECL: {
            FunctionDeclNode *fn = &node->as.function_decl;
            if (fn->name) {
                XaSymbol *sym = xa_analyzer_lookup(analyzer, fn->name);
                if (sym) {
                    generate_func_summary(analyzer, sym, node, out);
                }
            }
            // Walk body for nested functions
            if (fn->body)
                pass3_walk(analyzer, fn->body, out);
            break;
        }

        case AST_VAR_DECL: {
            VarDeclNode *vd = &node->as.var_decl;
            if (vd->name) {
                XaSymbol *sym = xa_analyzer_lookup(analyzer, vd->name);
                if (sym)
                    generate_var_hint(analyzer, sym, out);
            }
            break;
        }

        case AST_CLASS_DECL:
        case AST_STRUCT_DECL: {
            ClassDeclNode *cls = (node->type == AST_STRUCT_DECL)
                ? &node->as.struct_decl : &node->as.class_decl;
            // Walk methods for function summaries
            for (int i = 0; i < cls->method_count; i++) {
                if (cls->methods[i])
                    pass3_walk(analyzer, cls->methods[i], out);
            }
            break;
        }

        case AST_BLOCK: {
            for (int i = 0; i < node->as.block.count; i++)
                pass3_walk(analyzer, node->as.block.statements[i], out);
            break;
        }

        case AST_IF_STMT:
            pass3_walk(analyzer, node->as.if_stmt.then_branch, out);
            pass3_walk(analyzer, node->as.if_stmt.else_branch, out);
            break;

        case AST_WHILE_STMT:
            pass3_walk(analyzer, node->as.while_stmt.body, out);
            break;

        case AST_FOR_STMT:
            pass3_walk(analyzer, node->as.for_stmt.body, out);
            break;

        case AST_FOR_IN_STMT:
            pass3_walk(analyzer, node->as.for_in_stmt.body, out);
            break;

        case AST_EXPORT_STMT:
            if (node->as.export_stmt.declaration)
                pass3_walk(analyzer, node->as.export_stmt.declaration, out);
            break;

        default:
            break;
    }
}

/* ========== Public API ========== */

void xa_generate_jit_metadata(XaAnalyzer *analyzer, void *ast, XaJitMetadata *out) {
    if (!analyzer || !ast || !out) return;
    pass3_walk(analyzer, (AstNode *)ast, out);
}

XaFuncSummary *xa_jit_get_func_summary(XaJitMetadata *meta, uint32_t symbol_id) {
    if (!meta) return NULL;
    for (int i = 0; i < meta->func_count; i++) {
        if (meta->func_summaries[i].symbol_id == symbol_id)
            return &meta->func_summaries[i];
    }
    return NULL;
}

XaVarHint *xa_jit_get_var_hint(XaJitMetadata *meta, uint32_t symbol_id) {
    if (!meta) return NULL;
    for (int i = 0; i < meta->var_count; i++) {
        if (meta->var_hints[i].symbol_id == symbol_id)
            return &meta->var_hints[i];
    }
    return NULL;
}
