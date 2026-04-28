/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xanalyzer_visitor_stmt.c - Pass 2 statement type inference visitors
 *
 * KEY CONCEPT:
 *   Type inference for statements: variable declarations, assignments,
 *   control flow, loops, class/enum declarations, and import handling.
 */

#include "xanalyzer_visitor_internal.h"
#include "../../base/xchecks.h"

/* ============================================================================
 * Pass 2: Statement Visitors
 * ============================================================================
 * Type inference for statements: variable declarations, assignments,
 * control flow (if/while/for), and return statements.
 * ========================================================================== */

void xa_visit_var_decl_stmt(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node)
        return;

    VarDeclNode *var = &node->as.var_decl;
    XaSymbol *sym = xa_scope_lookup(ctx->analyzer->current_scope, var->name);
    if (!sym) {
        // Symbol not found (Pass 1 missed this declaration, e.g. inside for/while/if)
        // Define it now so type inference can proceed
        xa_visit_collect_var_decl(ctx, node);
        sym = xa_scope_lookup(ctx->analyzer->current_scope, var->name);
        if (!sym)
            return;
    }

    XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, sym);

    // P0-1: Variable must have type annotation or initializer
    if (!var->initializer && !links->declared_type) {
        XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
        char msg[256];
        snprintf(msg, sizeof(msg), "Variable '%s' must have a type annotation or initializer",
                 var->name);
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_MISSING_TYPE,
                                   msg, &loc);
    }

    // Infer type from initializer if no declared type
    XrType *var_type = NULL;
    if (var->initializer) {
        // Set expected_type for bidirectional inference
        XrType *saved_expected = ctx->expected_type;
        if (links->declared_type && !XR_TYPE_IS_UNKNOWN(links->declared_type)) {
            ctx->expected_type = links->declared_type;
        }
        XrType *init_type = xa_visit_infer_expr(ctx, var->initializer);
        ctx->expected_type = saved_expected;

        // Store inferred initializer type in the analyzer side table
        // (the canonical source for downstream codegen / LSP).
        xa_analyzer_set_node_type(ctx->analyzer, var->initializer, init_type);

        if (links->declared_type && !XR_TYPE_IS_UNKNOWN(links->declared_type)) {
            XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
            // Check null safety first (null→T, T?→T)
            bool null_err = xa_check_null_safety(ctx->analyzer, links->declared_type, init_type,
                                                 "Variable initializer", &loc);
            // Check assignment compatibility
            if (!null_err && !xa_typecheck_assignable(links->declared_type, init_type)) {
                // Json/JsonValue→concrete type: allowed at compile time, runtime check inserted by
                // codegen. e.g. let x: int = json["key"] is legal but requires runtime validation.
                if (!xr_is_json_coercion(links->declared_type, init_type)) {
                    char msg[256];
                    snprintf(msg, sizeof(msg), "Type '%s' is not assignable to type '%s'",
                             xr_type_to_string(init_type), xr_type_to_string(links->declared_type));
                    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                               XR_ERR_ANALYZE_TYPE_MISMATCH, msg, &loc);
                }
            }
            var_type = links->declared_type;
        } else {
            var_type = init_type;
            // Empty array literal without type annotation: require explicit type
            if (var->initializer && var->initializer->type == AST_ARRAY_LITERAL &&
                var->initializer->as.array_literal.count == 0 && XR_TYPE_IS_ARRAY(init_type) &&
                init_type->container.element_type &&
                XR_TYPE_IS_UNKNOWN(init_type->container.element_type)) {
                XrLocation loc = {
                    .file = ctx->file_path, .line = node->line, .column = node->column};
                xa_analyzer_add_diagnostic(
                    ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_TYPE_MISMATCH,
                    "Empty array '[]' requires a type annotation, e.g. let x: Array<int> = []",
                    &loc);
            }
        }
    } else if (links->declared_type) {
        var_type = links->declared_type;
    } else {
        // Fallback for missing type (error already reported above)
        var_type = xr_type_new_unknown(NULL);
    }

    links->type = var_type;

    // Track definite assignment
    // P1-3: Variables with type annotation are zero-initialized (703 type system rule)
    links->is_definitely_assigned = (var->initializer != NULL) || (links->declared_type != NULL);

    // JIT metadata: certainty level and const-foldable detection
    if (links->declared_type && !XR_TYPE_IS_UNKNOWN(links->declared_type)) {
        links->jit_certainty = XA_JIT_DECLARED;
    } else if (var_type && !XR_TYPE_IS_UNKNOWN(var_type)) {
        links->jit_certainty = XA_JIT_INFERRED;
    }
    // const with literal initializer = proven, can be constant-folded by JIT
    if (sym->is_const && var->initializer) {
        AstNodeType init_t = var->initializer->type;
        if (init_t == AST_LITERAL_INT || init_t == AST_LITERAL_FLOAT ||
            init_t == AST_LITERAL_STRING || init_t == AST_LITERAL_TRUE ||
            init_t == AST_LITERAL_FALSE || init_t == AST_LITERAL_NULL) {
            links->jit_certainty = XA_JIT_PROVEN;
            links->is_const_foldable = true;
        }
    }
    links->type_stability = XA_TYPE_STABLE;  // initial assignment is stable
    links->assign_count = var->initializer ? 1 : 0;

    // Detect loop variable context
    XaScope *s = ctx->analyzer->current_scope;
    while (s) {
        if (s->kind == XA_SCOPE_LOOP) {
            links->is_loop_variable = true;
            break;
        }
        s = s->parent;
    }

    // Store the inferred type in the analyzer side table for codegen.
    xa_analyzer_set_node_type(ctx->analyzer, node, var_type);

    // Create assignment flow node
    if (ctx->flow) {
        xa_flow_create_assignment(ctx->flow, NULL, var->name, var_type);
    }
}

void xa_visit_assignment_stmt(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node)
        return;

    AssignmentNode *assign = &node->as.assignment;
    XaSymbol *sym = xa_scope_lookup(ctx->analyzer->current_scope, assign->name);

    if (!sym) {
        XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
        char msg[128];
        snprintf(msg, sizeof(msg), "Undeclared variable '%s'", assign->name);
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_UNDEFINED_VAR,
                                   msg, &loc);
        return;
    }

    // Record write reference for Find References
    XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, sym);
    if (links) {
        uint32_t end_col = node->column + (assign->name ? strlen(assign->name) : 0);
        xa_symbol_add_ref(links, node->line, node->column, end_col, true);  // is_write=true
    }

    // Check const assignment
    if (sym->is_const) {
        XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
        char msg[128];
        snprintf(msg, sizeof(msg), "Cannot assign to const '%s'", assign->name);
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_CONST_ASSIGN,
                                   msg, &loc);
        return;
    }

    // Check in-parameter immutability: cannot reassign an 'in' parameter
    if (sym->kind == XA_SYM_PARAMETER && sym->passing_mode == XR_PARAM_IN) {
        XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
        char msg[128];
        snprintf(msg, sizeof(msg), "Cannot assign to 'in' parameter '%s' (readonly reference)",
                 assign->name);
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_CONST_ASSIGN,
                                   msg, &loc);
        return;
    }

    XrType *var_type = xa_analyzer_get_type(ctx->analyzer, sym);

    // Bidirectional inference: propagate target type to value expression
    XrType *saved_expected = ctx->expected_type;
    if (var_type && !XR_TYPE_IS_UNKNOWN(var_type)) {
        ctx->expected_type = var_type;
    }
    XrType *value_type = xa_visit_infer_expr(ctx, assign->value);
    ctx->expected_type = saved_expected;

    // Mark as definitely assigned + track type stability for JIT
    if (links) {
        links->is_definitely_assigned = true;
        links->assign_count++;
        // Detect type polymorphism: if assigned type differs from current type
        if (var_type && value_type && !XR_TYPE_IS_UNKNOWN(var_type) &&
            !XR_TYPE_IS_UNKNOWN(value_type)) {
            if (!xr_type_equals(var_type, value_type)) {
                links->type_stability = XA_TYPE_POLYMORPHIC;
            }
        }
    }

    if (!XR_TYPE_IS_UNKNOWN(var_type)) {
        XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
        // Check null safety first (null→T, T?→T)
        bool null_err =
            xa_check_null_safety(ctx->analyzer, var_type, value_type, "Assignment", &loc);
        if (!null_err && !xa_typecheck_assignable(var_type, value_type)) {
            // Json/JsonValue→concrete type: allowed at compile time, runtime check inserted by
            // codegen.
            if (!xr_is_json_coercion(var_type, value_type)) {
                char msg[256];
                snprintf(msg, sizeof(msg), "Type '%s' is not assignable to '%s' (type '%s')",
                         xr_type_to_string(value_type), assign->name, xr_type_to_string(var_type));
                xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                           XR_ERR_ANALYZE_TYPE_MISMATCH, msg, &loc);
            }
        }
    }

    // Update flow graph
    if (ctx->flow) {
        xa_flow_create_assignment(ctx->flow, NULL, assign->name, value_type);
    }
}

void xa_visit_if_stmt(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node)
        return;

    IfStmtNode *if_stmt = &node->as.if_stmt;

    // Analyze condition
    XrType *cond_type = xa_visit_infer_expr(ctx, if_stmt->condition);
    (void) cond_type;

    // Flow graph handles all type narrowing via TRUE_CONDITION / FALSE_CONDITION
    // nodes. apply_condition_narrowing() in xanalyzer_flow.c recognizes patterns:
    //   x != null, typeof(x) == "type", x is Type, truthiness, &&, ||
    // Early-return narrowing is automatic: when then-branch terminates,
    // its flow becomes unreachable → merge label only has the false-condition
    // path → opposite narrowing applies to subsequent code.

    XaFlowNode *saved = ctx->flow ? ctx->flow->current_flow : NULL;

    // Then branch: flow enters TRUE_CONDITION
    if (ctx->flow) {
        ctx->flow->current_flow = xa_flow_create_condition(ctx->flow, if_stmt->condition, true);
    }
    xa_visit_infer_stmt(ctx, if_stmt->then_branch);
    XaFlowNode *then_end = ctx->flow ? ctx->flow->current_flow : NULL;

    // Else branch: flow enters FALSE_CONDITION
    if (ctx->flow)
        ctx->flow->current_flow = saved;

    XaFlowNode *else_end = NULL;
    if (if_stmt->else_branch) {
        if (ctx->flow) {
            ctx->flow->current_flow =
                xa_flow_create_condition(ctx->flow, if_stmt->condition, false);
        }
        xa_visit_infer_stmt(ctx, if_stmt->else_branch);
        else_end = ctx->flow ? ctx->flow->current_flow : NULL;
    }

    // Merge branches
    if (ctx->flow) {
        XaFlowNode *merge = xa_flow_create_branch_label(ctx->flow);
        if (then_end)
            xa_flow_add_antecedent(merge, then_end);
        if (else_end) {
            xa_flow_add_antecedent(merge, else_end);
        } else {
            // No else: false-condition path flows through to merge
            ctx->flow->current_flow = saved;
            XaFlowNode *false_cond = xa_flow_create_condition(ctx->flow, if_stmt->condition, false);
            xa_flow_add_antecedent(merge, false_cond);
        }
        ctx->flow->current_flow = xa_flow_finish_label(ctx->flow, merge);
    }
}

void xa_visit_while_stmt(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node)
        return;

    WhileStmtNode *while_stmt = &node->as.while_stmt;

    // Create loop label
    XaFlowNode *loop_start = NULL;
    if (ctx->flow) {
        loop_start = xa_flow_create_loop_label(ctx->flow);
    }

    // Analyze condition
    xa_visit_infer_expr(ctx, while_stmt->condition);

    if (ctx->flow) {
        xa_flow_create_condition(ctx->flow, while_stmt->condition, true);
    }

    // Analyze body - inline block to match Pass 1 scope structure
    if (while_stmt->body) {
        if (while_stmt->body->type == AST_BLOCK) {
            BlockNode *blk = &while_stmt->body->as.block;
            for (int si = 0; si < blk->count; si++) {
                xa_visit_infer_stmt(ctx, blk->statements[si]);
            }
        } else {
            xa_visit_infer_stmt(ctx, while_stmt->body);
        }
    }

    // Back edge to loop start
    if (ctx->flow && loop_start) {
        xa_flow_add_antecedent(loop_start, ctx->flow->current_flow);
    }

    // Exit condition
    if (ctx->flow) {
        xa_flow_create_condition(ctx->flow, while_stmt->condition, false);
    }
}

void xa_visit_for_stmt(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node)
        return;

    ForStmtNode *for_stmt = &node->as.for_stmt;

    // Enter loop scope
    xa_analyzer_enter_scope(ctx->analyzer, XA_SCOPE_BLOCK, node);

    // Analyze initializer
    if (for_stmt->initializer) {
        xa_visit_infer_stmt(ctx, for_stmt->initializer);
    }

    // Create loop label
    if (ctx->flow) {
        xa_flow_create_loop_label(ctx->flow);
    }

    // Analyze condition
    if (for_stmt->condition) {
        xa_visit_infer_expr(ctx, for_stmt->condition);
    }

    // Analyze body - inline block to match Pass 1 scope structure
    if (for_stmt->body) {
        if (for_stmt->body->type == AST_BLOCK) {
            BlockNode *blk = &for_stmt->body->as.block;
            for (int si = 0; si < blk->count; si++) {
                xa_visit_infer_stmt(ctx, blk->statements[si]);
            }
        } else {
            xa_visit_infer_stmt(ctx, for_stmt->body);
        }
    }

    // Analyze increment
    if (for_stmt->increment) {
        xa_visit_infer_stmt(ctx, for_stmt->increment);
    }

    xa_analyzer_exit_scope(ctx->analyzer);
}

void xa_visit_return_stmt(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node)
        return;

    ReturnStmtNode *ret = &node->as.return_stmt;

    XrType *return_type = xr_type_new_void(NULL);

    if (ret->value_count == 0) {
        // No return value
        return_type = xr_type_new_void(NULL);
    } else if (ret->value_count == 1) {
        // Single return value
        if (ret->values[0]) {
            // Bidirectional inference: propagate declared return type to return expr
            // (e.g., return (x) => x + 1 inside fn(): fn(int): int)
            XrType *saved_expected = ctx->expected_type;
            if (ctx->expected_return_type && !XR_TYPE_IS_UNKNOWN(ctx->expected_return_type)) {
                ctx->expected_type = ctx->expected_return_type;
            } else {
                // Look up enclosing function's declared return type from scope
                XaScope *s = ctx->analyzer->current_scope;
                while (s && s->kind != XA_SCOPE_FUNCTION)
                    s = s->parent;
                if (s && s->ast_node) {
                    AstNode *fn_node = (AstNode *) s->ast_node;
                    XrType *decl_ret = NULL;
                    if (fn_node->type == AST_FUNCTION_DECL) {
                        decl_ret = (XrType *) fn_node->as.function_decl.return_type;
                    } else if (fn_node->type == AST_METHOD_DECL) {
                        decl_ret = (XrType *) fn_node->as.method_decl.return_type;
                    }
                    if (decl_ret && !XR_TYPE_IS_UNKNOWN(decl_ret)) {
                        ctx->expected_type = decl_ret;
                    }
                }
            }
            return_type = xa_visit_infer_expr(ctx, ret->values[0]);
            ctx->expected_type = saved_expected;
        }
    } else {
        // Multi-value return: create tuple type
        XrType **element_types = xr_malloc(sizeof(XrType *) * ret->value_count);
        for (int i = 0; i < ret->value_count; i++) {
            if (ret->values[i]) {
                element_types[i] = xa_visit_infer_expr(ctx, ret->values[i]);
            } else {
                element_types[i] = xr_type_new_unknown(NULL);
            }
        }
        // Create tuple type for multi-value return
        return_type = xr_type_new_tuple(ctx->analyzer->isolate, element_types, ret->value_count);

        // Store return type info in the analyzer side table.
        xa_analyzer_set_node_type(ctx->analyzer, node, return_type);

        xr_free(element_types);
    }

    // Collect return type for function inference
    xa_infer_add_return_type(ctx, return_type);

    // Check against expected return type (strict: void and concrete types enforced)
    if (ctx->expected_return_type && !XR_TYPE_IS_UNKNOWN(ctx->expected_return_type)) {
        if (!xa_typecheck_assignable(ctx->expected_return_type, return_type)) {
            // Json/JsonValue→primitive/union: allowed with runtime type check (OP_CHECKTYPE)
            if (!xr_is_json_coercion(ctx->expected_return_type, return_type)) {
                XrLocation loc = {
                    .file = ctx->file_path, .line = node->line, .column = node->column};
                char msg[256];
                xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                           XR_ERR_ANALYZE_TYPE_MISMATCH, "Return type mismatch",
                                           &loc);
            }
        }
    }

    // Mark flow as unreachable after return
    if (ctx->flow) {
        ctx->flow->current_flow = ctx->flow->unreachable_flow;
    }
}

void xa_visit_block_stmt(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node)
        return;

    xa_analyzer_enter_scope(ctx->analyzer, XA_SCOPE_BLOCK, node);

    BlockNode *block = &node->as.block;
    for (int i = 0; i < block->count; i++) {
        xa_visit_infer_stmt(ctx, block->statements[i]);
    }

    xa_analyzer_exit_scope(ctx->analyzer);
}
