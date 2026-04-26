/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xstmt_function.c - Xray function definition statement compilation
 *
 * KEY CONCEPT:
 *   - Local functions
 *   - Global functions
 *   - Anonymous functions
 *   - Closures and Upvalue management
 */

#include "xstmt.h"
#include "../../base/xchecks.h"
#include "xcompiler.h"
#include "xcompiler_context.h"
#include "xemit.h"
#include "xexpr.h"       // xr_compile_expr, xexpr_to_specific_reg
#include "xexpr_desc.h"  // XrExprDesc
#include "../../runtime/value/xtype.h"
#include "../parser/xast_nodes.h"
#include "../analyzer/xanalyzer_symbol.h"
#include "xcompiler_class_registry.h"
#include "../../runtime/value/xstruct_layout.h"
#include <stdio.h>
#include <string.h>

// External functions (need to be public in xcompiler.h)
// - xr_compiler_init, xr_compiler_end
// - xr_vm_proto_add_proto

// ========== Function Definition ==========

/*
 * Pre-declare function name only (for hoisting support)
 * This enables mutual recursion by registering all function names
 * before compiling any function bodies.
 */
void compile_function_decl_only(XrCompilerContext *ctx, XrCompiler *compiler, FunctionDeclNode *node) {
    XR_DCHECK(ctx != NULL, "compile_function_decl_only: NULL ctx");
    XR_DCHECK(compiler != NULL, "compile_function_decl_only: NULL compiler");
    if (!node || !node->name) return;

    bool is_module_level = (compiler->scope_depth == 0 && compiler->type == FUNCTION_SCRIPT);

    // REPL mode: top-level functions use shared variables (persist across inputs)
    if (is_repl_top_level(ctx, compiler)) {
        XrString *name_str = xr_compile_time_intern(ctx->X, node->name, strlen(node->name));
        // Pre-allocate shared slot (shared_add handles redefine in repl_mode)
        int shared_index = shared_add(ctx, compiler, name_str);
        shared_set_const(ctx, shared_index, true);
        (void)shared_index;
        return;
    }

    if (compiler->scope_depth > 0 || is_module_level) {
        XrString *name_str = xr_compile_time_intern(ctx->X, node->name, strlen(node->name));

        // Check if already defined (avoid duplicate)
        XrLocalInfo *existing = compiler_get_local_by_name(compiler, node->name);
        if (existing) return;  // Already hoisted

        // Pre-define as local, will be assigned actual closure later
        XrLocalInfo *local = scope_define_local(ctx, compiler, name_str);
        local->is_const = true;
        local->is_hoisted = true;  // Mark as hoisted (value not yet assigned)

        // For module-level functions, also pre-create the shared var entry so that
        // compile_function can find it via shared_get_or_add without re-adding.
        if (is_module_level) {
            int si = shared_get_or_add(ctx, compiler, name_str);
            shared_set_const(ctx, si, true);
        }
    }
}

/* ========== Pre-scan ========== */

// Add name to captured set (avoid duplicates)
static void ps_mark_captured(XrCompiler *compiler, const char *name) {
    for (int i = 0; i < compiler->prescan_captured.count; i++) {
        if (compiler->prescan_captured.data[i] &&
            strcmp(compiler->prescan_captured.data[i], name) == 0) return;
    }
    XR_AVEC_PUSH(compiler->arena, compiler->prescan_captured, name);
}

// Unified tree walk for pre-scan.
// depth=0: outer function scope — collect var names AND find inner functions.
// depth>0: inside a nested function — check variable references against names[].
// names[]/ncount is the accumulated set of outer-scope variable names.
// When a FUNCTION_DECL/EXPR is found at any depth, we enter it at depth+1 to find
// all references (transitively), so deep nesting works correctly.
static void ps_walk(AstNode *node, const char **names, int *ncount,
                    XrCompiler *compiler, int depth) {
    if (!node) return;
    switch (node->type) {

    // ---- Leaf nodes (no children) ----
    case AST_LITERAL_INT: case AST_LITERAL_FLOAT: case AST_LITERAL_BIGINT:
    case AST_LITERAL_STRING: case AST_LITERAL_NULL: case AST_LITERAL_TRUE:
    case AST_LITERAL_FALSE: case AST_BREAK_STMT: case AST_CONTINUE_STMT:
    case AST_THIS_EXPR: case AST_YIELD_STMT: case AST_CANCELLED_EXPR: case AST_MOVE_EXPR:
        break;

    // ---- Variable reference ----
    case AST_VARIABLE:
        if (depth > 0) {
            for (int i = 0; i < *ncount; i++) {
                if (names[i] && strcmp(names[i], node->as.variable.name) == 0) {
                    ps_mark_captured(compiler, names[i]);
                    break;
                }
            }
        }
        break;

    // ---- Variable declarations (add names at depth=0) ----
    case AST_VAR_DECL: case AST_CONST_DECL:
        if (depth == 0 && node->as.var_decl.name && *ncount < 256)
            names[(*ncount)++] = node->as.var_decl.name;
        ps_walk(node->as.var_decl.initializer, names, ncount, compiler, depth);
        break;
    case AST_MULTI_VAR_DECL:
        if (depth == 0) {
            for (int i = 0; i < node->as.multi_var_decl.name_count && *ncount < 256; i++)
                if (node->as.multi_var_decl.names[i])
                    names[(*ncount)++] = node->as.multi_var_decl.names[i];
        }
        for (int i = 0; i < node->as.multi_var_decl.value_count; i++)
            ps_walk(node->as.multi_var_decl.values[i], names, ncount, compiler, depth);
        break;
    case AST_DESTRUCTURE_DECL:
        if (depth == 0 && node->as.destructure_decl.pattern) {
            XrDestructurePattern *pat = node->as.destructure_decl.pattern;
            if (pat->type == 2 && *ncount < 256)
                if (pat->as.identifier.name) names[(*ncount)++] = pat->as.identifier.name;
        }
        break;

    // ---- Nested functions: record name (depth=0), enter body at depth+1 ----
    case AST_FUNCTION_DECL:
        if (depth == 0 && node->as.function_decl.name && *ncount < 256)
            names[(*ncount)++] = node->as.function_decl.name;
        if (node->as.function_decl.body)
            ps_walk(node->as.function_decl.body, names, ncount, compiler, depth + 1);
        break;
    case AST_FUNCTION_EXPR:
        if (node->as.function_expr.body)
            ps_walk(node->as.function_expr.body, names, ncount, compiler, depth + 1);
        break;

    // ---- Expressions ----
    case AST_BINARY_ADD: case AST_BINARY_SUB: case AST_BINARY_MUL:
    case AST_BINARY_DIV: case AST_BINARY_MOD: case AST_BINARY_BAND:
    case AST_BINARY_BOR: case AST_BINARY_BXOR: case AST_BINARY_LSHIFT:
    case AST_BINARY_RSHIFT: case AST_BINARY_EQ: case AST_BINARY_NE:
    case AST_BINARY_EQ_STRICT: case AST_BINARY_NE_STRICT: case AST_BINARY_LT:
    case AST_BINARY_LE: case AST_BINARY_GT: case AST_BINARY_GE:
    case AST_BINARY_AND: case AST_BINARY_OR:
    case AST_NULLISH_COALESCE:
        ps_walk(node->as.binary.left, names, ncount, compiler, depth);
        ps_walk(node->as.binary.right, names, ncount, compiler, depth);
        break;
    case AST_OPTIONAL_CHAIN:
        ps_walk(node->as.optional_chain.object, names, ncount, compiler, depth);
        if (node->as.optional_chain.index)
            ps_walk(node->as.optional_chain.index, names, ncount, compiler, depth);
        break;
    case AST_UNARY_NEG: case AST_UNARY_NOT: case AST_UNARY_BNOT:
    case AST_FORCE_UNWRAP:
        ps_walk(node->as.unary.operand, names, ncount, compiler, depth);
        break;
    case AST_GROUPING:
        ps_walk(node->as.grouping, names, ncount, compiler, depth);
        break;
    case AST_TERNARY:
        ps_walk(node->as.ternary.condition, names, ncount, compiler, depth);
        ps_walk(node->as.ternary.true_expr, names, ncount, compiler, depth);
        ps_walk(node->as.ternary.false_expr, names, ncount, compiler, depth);
        break;
    case AST_ASSIGNMENT:
        if (depth > 0) {
            for (int i = 0; i < *ncount; i++) {
                if (names[i] && strcmp(names[i], node->as.assignment.name) == 0)
                    ps_mark_captured(compiler, names[i]);
            }
        }
        ps_walk(node->as.assignment.value, names, ncount, compiler, depth);
        break;
    case AST_COMPOUND_ASSIGNMENT:
        if (depth > 0) {
            for (int i = 0; i < *ncount; i++) {
                if (names[i] && strcmp(names[i], node->as.compound_assignment.name) == 0)
                    ps_mark_captured(compiler, names[i]);
            }
        }
        ps_walk(node->as.compound_assignment.value, names, ncount, compiler, depth);
        break;
    case AST_INC: case AST_DEC:
        if (depth > 0) {
            for (int i = 0; i < *ncount; i++) {
                if (names[i] && strcmp(names[i], node->as.inc.name) == 0)
                    ps_mark_captured(compiler, names[i]);
            }
        }
        break;
    case AST_CALL_EXPR:
        ps_walk(node->as.call_expr.callee, names, ncount, compiler, depth);
        for (int i = 0; i < node->as.call_expr.arg_count; i++)
            ps_walk(node->as.call_expr.arguments[i], names, ncount, compiler, depth);
        break;
    case AST_INDEX_GET:
        ps_walk(node->as.index_get.array, names, ncount, compiler, depth);
        ps_walk(node->as.index_get.index, names, ncount, compiler, depth);
        break;
    case AST_INDEX_SET:
        ps_walk(node->as.index_set.array, names, ncount, compiler, depth);
        ps_walk(node->as.index_set.index, names, ncount, compiler, depth);
        ps_walk(node->as.index_set.value, names, ncount, compiler, depth);
        break;
    case AST_MEMBER_ACCESS:
        ps_walk(node->as.member_access.object, names, ncount, compiler, depth);
        break;
    case AST_MEMBER_SET:
        ps_walk(node->as.member_set.object, names, ncount, compiler, depth);
        ps_walk(node->as.member_set.value, names, ncount, compiler, depth);
        break;
    case AST_ARRAY_LITERAL:
        for (int i = 0; i < node->as.array_literal.count; i++)
            ps_walk(node->as.array_literal.elements[i], names, ncount, compiler, depth);
        break;
    case AST_TEMPLATE_STRING:
        for (int i = 0; i < node->as.template_str.part_count; i++)
            ps_walk(node->as.template_str.parts[i], names, ncount, compiler, depth);
        break;
    case AST_NEW_EXPR:
        for (int i = 0; i < node->as.new_expr.arg_count; i++)
            ps_walk(node->as.new_expr.arguments[i], names, ncount, compiler, depth);
        break;
    case AST_RANGE:
        ps_walk(node->as.range.start, names, ncount, compiler, depth);
        ps_walk(node->as.range.end, names, ncount, compiler, depth);
        break;
    case AST_AS_EXPR:
        ps_walk(node->as.as_expr.expr, names, ncount, compiler, depth);
        break;
    case AST_IS_EXPR:
        ps_walk(node->as.is_expr.expr, names, ncount, compiler, depth);
        break;
    case AST_GO_EXPR:
        if (node->as.go_expr.expr)
            ps_walk(node->as.go_expr.expr, names, ncount, compiler, depth);
        break;
    case AST_AWAIT_EXPR:
        ps_walk(node->as.await_expr.expr, names, ncount, compiler, depth);
        break;

    // ---- Statements ----
    case AST_BLOCK:
        for (int i = 0; i < node->as.block.count; i++)
            ps_walk(node->as.block.statements[i], names, ncount, compiler, depth);
        break;
    case AST_EXPR_STMT:
        ps_walk(node->as.expr_stmt, names, ncount, compiler, depth);
        break;
    case AST_PRINT_STMT:
        for (int i = 0; i < node->as.print_stmt.expr_count; i++)
            ps_walk(node->as.print_stmt.exprs[i], names, ncount, compiler, depth);
        break;
    case AST_RETURN_STMT:
        for (int i = 0; i < node->as.return_stmt.value_count; i++)
            ps_walk(node->as.return_stmt.values[i], names, ncount, compiler, depth);
        break;
    case AST_THROW_STMT:
        ps_walk(node->as.throw_stmt.expression, names, ncount, compiler, depth);
        break;
    case AST_IF_STMT:
        ps_walk(node->as.if_stmt.condition, names, ncount, compiler, depth);
        ps_walk(node->as.if_stmt.then_branch, names, ncount, compiler, depth);
        ps_walk(node->as.if_stmt.else_branch, names, ncount, compiler, depth);
        break;
    case AST_WHILE_STMT:
        ps_walk(node->as.while_stmt.condition, names, ncount, compiler, depth);
        ps_walk(node->as.while_stmt.body, names, ncount, compiler, depth);
        break;
    case AST_FOR_STMT:
        ps_walk(node->as.for_stmt.initializer, names, ncount, compiler, depth);
        ps_walk(node->as.for_stmt.condition, names, ncount, compiler, depth);
        ps_walk(node->as.for_stmt.increment, names, ncount, compiler, depth);
        ps_walk(node->as.for_stmt.body, names, ncount, compiler, depth);
        break;
    case AST_FOR_IN_STMT:
        if (depth == 0) {
            if (node->as.for_in_stmt.item_name && *ncount < 256)
                names[(*ncount)++] = node->as.for_in_stmt.item_name;
            if (node->as.for_in_stmt.value_name && *ncount < 256)
                names[(*ncount)++] = node->as.for_in_stmt.value_name;
        }
        ps_walk(node->as.for_in_stmt.collection, names, ncount, compiler, depth);
        ps_walk(node->as.for_in_stmt.body, names, ncount, compiler, depth);
        break;
    case AST_TRY_CATCH:
        if (depth == 0 && node->as.try_catch.catch_var && *ncount < 256)
            names[(*ncount)++] = node->as.try_catch.catch_var;
        ps_walk(node->as.try_catch.try_body, names, ncount, compiler, depth);
        ps_walk(node->as.try_catch.catch_body, names, ncount, compiler, depth);
        ps_walk(node->as.try_catch.finally_body, names, ncount, compiler, depth);
        break;
    case AST_SCOPE_BLOCK:
        ps_walk(node->as.scope_block.body, names, ncount, compiler, depth);
        break;
    default:
        break;
    }
}

// Public entry: fill compiler->prescan_captured with names of locals that will
// be captured by any (possibly transitive) nested function.
// Must be called BEFORE scope_define_local_reg for params.
// Params are seeded from fn_node->params so they are available for capture detection.
void prescan_fn_body(XrCompiler *compiler, FunctionDeclNode *fn_node, AstNode *body) {
    if (!body || !compiler) return;
    XR_AVEC_INIT(compiler->prescan_captured);

    // Seed names with the function's parameter names
    const char *names[256];
    int ncount = 0;
    if (fn_node) {
        for (int i = 0; i < fn_node->param_count && ncount < 256; i++) {
            XrParamNode *p = fn_node->params[i];
            if (p && p->name) names[ncount++] = p->name;
        }
    }

    ps_walk(body, names, &ncount, compiler, 0);
}

/*
 * Compile function definition statement
 *
 * Handles:
 * - Local functions (can recursively access self)
 * - Global functions
 * - Anonymous function expressions
 * - Parameter definitions
 * - Closure creation
 */
void compile_function(XrCompilerContext *ctx, XrCompiler *compiler, FunctionDeclNode *node) {
    XR_DCHECK(ctx != NULL, "compile_function: NULL ctx");
    XR_DCHECK(compiler != NULL, "compile_function: NULL compiler");
    XR_DCHECK(node != NULL, "compile_function: NULL node");
    // Save function definition line for CLOSURE instruction (set by xr_compile_statement)
    int func_def_line = ctx->current_line;

    // If named function, first define function name in current scope
    int func_reg = -1;
    XrString *name_str = NULL;

    /*
     * Module system optimization: module-level functions compile as local variables
     * Condition: scope_depth == 0 and type == FUNCTION_SCRIPT
     */
    bool is_module_level = (compiler->scope_depth == 0 && compiler->type == FUNCTION_SCRIPT);

    // REPL mode: top-level functions stored in shared variables
    bool repl_top_level = is_repl_top_level(ctx, compiler);
    int shared_fn_index = -1;

    if (repl_top_level && node->name != NULL) {
        name_str = xr_compile_time_intern(ctx->X, node->name, strlen(node->name));
        shared_fn_index = shared_get_in_scope(ctx, compiler, name_str);
        // Allocate a temp register for the closure
        func_reg = reg_alloc(ctx, compiler);
    } else if (node->name != NULL && (compiler->scope_depth > 0 || is_module_level)) {
        // Local function or module-level function (non-shared): define first, support recursion
        name_str = xr_compile_time_intern(ctx->X, node->name, strlen(node->name));

        // Check if already hoisted (from two-pass compilation)
        XrLocalInfo *existing = compiler_get_local_by_name(compiler, node->name);
        if (existing && existing->is_hoisted) {
            // Already pre-declared, just use its register.
            // Keep is_hoisted=true so emit_ctx_sync_before_closure emits
            // LOADNULL before CELL_NEW (forward reference support).
            // Cleared after CLOSURE emission below.
            func_reg = existing->reg;
        } else if (existing && existing->is_const) {
            xr_compiler_error(ctx, compiler, "cannot redefine constant '%s'", node->name);
            return;
        } else {
            // Not hoisted, define now
            XrLocalInfo *local = scope_define_local(ctx, compiler, name_str);
            local->is_const = true;  // fn-declared functions cannot be overwritten
            func_reg = local->reg;
        }
    }

    // Create new compiler (nested)
    XrCompiler function_compiler;
    xr_compiler_init(ctx, &function_compiler, FUNCTION_FUNCTION);
    function_compiler.enclosing = compiler;

    // Set function name
    if (node->name != NULL) {
        if (name_str == NULL) {
            name_str = xr_compile_time_intern(ctx->X, node->name, strlen(node->name));
        }
        function_compiler.proto->name = name_str;
    }

    // Check if has rest parameter
    bool has_rest = false;
    for (int i = 0; i < node->param_count; i++) {
        if (node->params[i] && node->params[i]->is_rest) {
            has_rest = true;
            break;
        }
    }
    function_compiler.proto->is_vararg = has_rest;

    // Set entry type: controls VM dispatch at function entry
    if (node->is_generator) {
        function_compiler.proto->entry_type = XR_ENTRY_GENERATOR;
    } else if (node->required_count < (has_rest ? node->param_count - 1 : node->param_count)) {
        function_compiler.proto->entry_type = XR_ENTRY_DEFAULTS;
    } else {
        function_compiler.proto->entry_type = XR_ENTRY_NORMAL;
    }

    // Set parameter count: numparams excludes rest param (VM packs varargs into it)
    function_compiler.proto->numparams = has_rest ? node->param_count - 1 : node->param_count;
    function_compiler.proto->min_params = node->required_count;

    // Set return type (if specified)
    if (node->return_type != NULL) {
        const char *type_str = xr_type_to_string(node->return_type);
        function_compiler.proto->return_type = type_str ? strdup(type_str) : NULL;
        function_compiler.declared_return_type = node->return_type;
    }

    // Set test attributes (if any)
    if (node->attr_count > 0 && node->attributes != NULL) {
        XrAttribute *attr = node->attributes[0];
        function_compiler.proto->test_attr = (uint8_t)attr->kind;
        function_compiler.proto->test_timeout = attr->timeout;
    }

    // Enter function scope
    scope_begin(&function_compiler);

    // Pre-scan BEFORE defining params: determines which params/locals will be
    // captured by nested functions, so scope_define_local_reg can eagerly
    // mark them as captured. This finalises captured_count before any
    // codegen, making depth calculation in scope_resolve_upvalue exact.
    prescan_fn_body(&function_compiler, node, node->body);

    // Define parameters as local variables (directly use registers 0, 1, 2...)
    for (int i = 0; i < node->param_count; i++) {
        XrParamNode *param = node->params[i];
        if (!param) continue;
        XrString *param_str = xr_compile_time_intern(ctx->X, param->name, strlen(param->name));
        XrLocalInfo *local = scope_define_local_reg(ctx, &function_compiler, param_str, i);
        // Set parameter's compile-time type (for native I64/F64 codegen)
        if (local && param->type) {
            local_set_compile_type(local, param->type);
        } else if (local && !param->type && node->name && ctx->analyzer) {
            // No explicit annotation: read call-site inferred type from Analyzer
            XaSymbol *fn_sym = xa_analyzer_lookup(ctx->analyzer, node->name);
            if (fn_sym) {
                XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, fn_sym);
                if (links && links->inferred_param_types &&
                    i < links->inferred_param_count) {
                    XrType *pt = links->inferred_param_types[i];
                    if (pt && !XR_TYPE_IS_UNKNOWN(pt)) {
                        // Inferred object types from single-pass analysis may be
                        // incomplete for recursive functions (recursive call args
                        // are often unknown and skipped). Make them nullable as a
                        // safe default — Json fields can always be null at runtime.
                        if (XR_TYPE_IS_JSON(pt) && !XR_TYPE_IS_NULLABLE(pt)) {
                            pt = xr_type_make_nullable(ctx->X, pt);
                        }
                        local_set_compile_type(local, pt);
                    }
                }
            }
        }
    }

    // After parameter definition, freereg should equal local_end
    if (function_compiler.regalloc) {
        xreg_set_freereg(function_compiler.regalloc, xreg_get_local_end(function_compiler.regalloc));
    }

    // Save return type info (single source of truth for JIT return type)
    // Parameter types are carried via param_types (generated in xr_compiler_end)
    {
        if (node->return_type != NULL) {
            function_compiler.proto->return_type_info = (struct XrType *)node->return_type;
        }
        // No explicit return annotation: read inferred return type from Analyzer
        else if (node->name && ctx->analyzer) {
            XaSymbol *fn_sym = xa_analyzer_lookup(ctx->analyzer, node->name);
            if (fn_sym) {
                XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, fn_sym);
                if (links && links->return_type && !XR_TYPE_IS_UNKNOWN(links->return_type)
                    && !XR_TYPE_IS_VOID(links->return_type)) {
                    function_compiler.proto->return_type_info = (struct XrType *)links->return_type;
                }
            }
        }
    }

    // Generate default parameter initialization code
    for (int i = 0; i < node->param_count; i++) {
        XrParamNode *param = node->params[i];
        if (param && param->default_value != NULL) {
            int param_reg = i;

            // Load null to temp register for comparison
            int temp_reg = reg_alloc(ctx, &function_compiler);
            emit_abc(function_compiler.emitter, OP_LOADNULL, temp_reg, 0, 0);

            // Compare param with null
            emit_abc(function_compiler.emitter, OP_EQ, param_reg, temp_reg, 0);
            reg_free(&function_compiler, temp_reg);

            // Jump over default assignment if param != null
            int skip_jmp = emit_jump(function_compiler.emitter, OP_JMP);

            // Compile default value expression
            XrExprDesc default_desc = xr_compile_expr(ctx, &function_compiler, param->default_value);
            xexpr_to_specific_reg(ctx, &function_compiler, &default_desc, param_reg);

            // Patch jump
            patch_jump(function_compiler.emitter, skip_jmp, -1);
        }
    }

    // Struct value semantics: copy struct-typed parameters at function entry
    // so callee gets an independent copy (caller's struct is not modified)
    // Skip for in/ref params (they pass by reference, no copy needed)
    for (int i = 0; i < node->param_count; i++) {
        XrParamNode *param = node->params[i];
        if (!param) continue;
        if (param->passing_mode != XR_PARAM_VALUE) continue;
        XrLocalInfo *plocal = compiler_get_local_by_name(&function_compiler, param->name);
        if (!plocal) continue;
        // Check compile_type directly (set by analyzer or type annotation)
        bool is_vt = plocal->compile_type && plocal->compile_type->is_value_type;
        // Fallback: look up class symbol in analyzer for type annotations
        // that were created by parser (missing is_value_type)
        if (!is_vt && plocal->compile_type && ctx->analyzer) {
            const char *cname = NULL;
            if (plocal->compile_type->kind == XR_KIND_CLASS ||
                plocal->compile_type->kind == XR_KIND_INSTANCE) {
                cname = plocal->compile_type->instance.class_name;
            }
            if (cname) {
                XaSymbol *csym = xa_analyzer_lookup(ctx->analyzer, cname);
                if (csym && csym->kind == XA_SYM_CLASS) {
                    XaSymbolLinks *clinks = xa_analyzer_get_links(ctx->analyzer, csym);
                    if (clinks && clinks->type && clinks->type->is_value_type) {
                        is_vt = true;
                        plocal->compile_type->is_value_type = true;
                    }
                }
            }
        }
        if (is_vt) {
            // Struct with layout -> OP_STRUCT_COPY
            bool used_struct_copy = false;
            if (plocal->compile_type && ctx->class_registry) {
                const char *cn = plocal->compile_type->instance.class_name;
                if (cn) {
                    ClassInfo *ci = xr_class_registry_lookup(ctx->class_registry, cn);
                    if (ci && ci->struct_layout) {
                        int sz = (8 + ci->struct_layout->total_size + 15) & ~15;
                        int slot = function_compiler.struct_area_offset / 16;
                        function_compiler.struct_area_offset += sz;
                        emit_abc(function_compiler.emitter, OP_STRUCT_COPY,
                                 plocal->reg, plocal->reg, slot);
                        used_struct_copy = true;
                    }
                }
            }
            if (!used_struct_copy)
                emit_abc(function_compiler.emitter, OP_COPY, plocal->reg, plocal->reg, 0);
        }
    }

    // Compile function body
    xr_compile_statement(ctx, &function_compiler, node->body);

    // End compilation
    XrProto *proto = xr_compiler_end(ctx, &function_compiler);

    /*
     * If nested compilation fails, don't propagate error to outer compiler
     * This way subsequent function definitions can still compile (supports mutual recursion etc)
     * Final error will be caught in xr_compiler_end by checking ctx->had_error
     */
    if (proto == NULL) {
        // Mark context has error, but don't block subsequent compilation
        xr_compiler_ctx_set_error(ctx);
        return;
    }

    if (proto != NULL) {
        // Add function prototype to parent compiler
        int proto_idx = xr_vm_proto_add_proto(compiler->proto, proto);

        // Restore function definition line for CLOSURE instruction
        ctx->current_line = func_def_line;

        // Check if pure function (no upvalue)
        bool is_pure_function = (PROTO_UPVAL_COUNT(proto) == 0);

        // Define function based on scope type
        if (node->name != NULL) {
            /*
             * REPL mode: all top-level functions go to shared_array
             * Module-level pure functions stored in shared_array (global heap, coroutine-safe)
             * Functions with upvalue keep as local variables (need closure capture)
             */
            if (repl_top_level) {
                // REPL: emit closure to temp reg, store in shared variable
                emit_ctx_sync_before_closure(ctx, compiler);
                emit_abx(compiler->emitter, OP_CLOSURE, func_reg, proto_idx);
                emit_abx(compiler->emitter, OP_SETSHARED, func_reg, shared_fn_index);
                reg_free(compiler, func_reg);
            } else if (is_module_level && is_pure_function) {
                // Pure function: store in shared_array (cross-coroutine access), keep local variable (recursive call)
                emit_ctx_sync_before_closure(ctx, compiler);
                emit_abx(compiler->emitter, OP_CLOSURE, func_reg, proto_idx);

                int shared_index = shared_get_or_add(ctx, compiler, name_str);
                shared_set_const(ctx, shared_index, true);
                emit_abx(compiler->emitter, OP_SETSHARED, func_reg, shared_index);

                // Inner function access prioritizes shared over upvalue, so uses OP_GETSHARED
            } else if (is_module_level) {
                // Module-level function with upvalue: local + shared
                emit_ctx_sync_before_closure(ctx, compiler);
                emit_abx(compiler->emitter, OP_CLOSURE, func_reg, proto_idx);

                int shared_index = shared_get_or_add(ctx, compiler, name_str);
                shared_set_const(ctx, shared_index, true);
                emit_abx(compiler->emitter, OP_SETSHARED, func_reg, shared_index);

                // Associate Proto with local variable (for coroutine safety check)
                XrLocalInfo *local = compiler_get_local_by_name(compiler, node->name);
                if (local) {
                    local->is_closure = true;
                    local->closure_proto = proto;
                }
            } else if (compiler->scope_depth > 0) {
                // Local function: local variable only
                emit_ctx_sync_before_closure(ctx, compiler);

                XrLocalInfo *local = compiler_get_local_by_name(compiler, node->name);
                if (local && local->is_cellified) {
                    // Cell already exists in func_reg (created by emit_ctx_sync).
                    // Emit closure to temp, then update cell via CELL_SET.
                    int tmp = reg_alloc(ctx, compiler);
                    emit_abx(compiler->emitter, OP_CLOSURE, tmp, proto_idx);
                    emit_abc(compiler->emitter, OP_CELL_SET, func_reg, tmp, 0);
                    reg_free(compiler, tmp);
                    local->is_closure = true;
                    local->closure_proto = proto;
                    local->is_hoisted = false;
                } else {
                    emit_abx(compiler->emitter, OP_CLOSURE, func_reg, proto_idx);
                    if (local) {
                        local->is_closure = true;
                        local->closure_proto = proto;
                        local->is_hoisted = false;
                        // Cellify if captured mutable (const fn decls use raw snapshot)
                        if (!local->is_const && local->is_captured && local->ctx_slot >= 0 && !local->is_cellified) {
                            emit_abc(compiler->emitter, OP_CELL_NEW, func_reg, 0, 0);
                            local->is_cellified = true;
                        }
                    }
                }
            } else if (compiler->scope_depth == 0) {
                // Non-module top-level (like REPL) - use shared variable

                // Check if constant with same name already exists
                int existing = shared_get_in_scope(ctx, compiler, name_str);
                if (existing >= 0 && shared_is_const(ctx, existing)) {
                    xr_compiler_error(ctx, compiler, "cannot redefine function '%s'", node->name);
                    return;
                }

                int reg = reg_alloc(ctx, compiler);
                emit_ctx_sync_before_closure(ctx, compiler);
                emit_abx(compiler->emitter, OP_CLOSURE, reg, proto_idx);

                int shared_index = shared_get_or_add(ctx, compiler, name_str);
                shared_set_const(ctx, shared_index, true);
                emit_abx(compiler->emitter, OP_SETSHARED, reg, shared_index);
                reg_free(compiler, reg);
            }
        } else {
            // Anonymous function expression
            int reg = reg_alloc(ctx, compiler);
            emit_ctx_sync_before_closure(ctx, compiler);
            emit_abx(compiler->emitter, OP_CLOSURE, reg, proto_idx);
        }
    }
}

