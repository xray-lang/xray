/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_task_call.inc.c - Direct Task creation and owned await successors
 *
 * KEY CONCEPT:
 *   Creation prepares ordinary arguments once; awaiting never executes a synchronous call.
 */
static bool source_go_target(SourceContext *ctx, AstNode *node, SourceName **binding, SourceName **target) {
    GoExprNode *go = &node->as.go_expr;
    AstNode *call = go->expr;
    if (go->name || go->link_mode || go->spawn_kind != XR_SPAWN_COROUTINE || !call || call->type != AST_CALL_EXPR)
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "go requires an ordinary direct function call");
    AstNode *callee = call->as.call_expr.callee;
    for (uint32_t depth = 0; callee && callee->type == AST_GROUPING; ++depth) {
        if (depth >= 128) return source_fail(ctx, node, XR_XIR_BUDGET, "go declaration path depth exhausted");
        if (!source_work(ctx, callee)) return false;
        callee = callee->as.grouping;
    }
    *binding = NULL; *target = NULL;
    if (callee && callee->type == AST_VARIABLE) {
        *binding = visible_name(ctx, callee->as.variable.name); *target = *binding;
        if (*target && (*target)->kind == SOURCE_IMPORT)
            *target = imported_declaration(ctx, *target, (*target)->imported);
    } else if (callee && callee->type == AST_MEMBER_ACCESS && callee->as.member_access.object &&
        callee->as.member_access.object->type == AST_VARIABLE) {
        *binding = visible_name(ctx, callee->as.member_access.object->as.variable.name);
        if (*binding && (*binding)->kind == SOURCE_MODULE)
            *target = imported_declaration(ctx, *binding, callee->as.member_access.name);
    }
    if (!*target || (*target)->kind != SOURCE_FUNCTION)
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "go requires an accessible ordinary function declaration");
    return true;
}
static bool source_go_call(SourceContext *ctx, AstNode *node, SourceExpectedType context, SourceValue *value) {
    SourceName *binding, *target;
    if (!source_go_target(ctx, node, &binding, &target)) return false;
    AstNode *call = node->as.go_expr.expr;
    SourceCallSyntax syntax;
    if (!source_call_syntax(ctx, call, &syntax)) return false;
    for (int a = 0; a < syntax.arg_count; ++a) {
        if (!source_work(ctx, node)) return false;
        if (syntax.arg_accesses && syntax.arg_accesses[a] != XR_CALL_ARG_PLAIN)
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "go argument access mode is not admitted");
    }
    SourceExpectedType child = ctx->active_expression->task_element_context;
    XrXirType expected = context.type;
    for (uint32_t depth = 0; context.present && xr_xir_type_is_nullable(&ctx->types, expected); ++depth) {
        if (depth >= 128) return source_fail(ctx, node, XR_XIR_BUDGET, "go result context depth exhausted");
        if (!source_work(ctx, node)) return false;
        expected = xr_xir_nullable_element(&ctx->types, expected);
    }
    const XrXirTypeNode *task_context = context.present ? xr_xir_type_node(&ctx->types, expected) : NULL;
    if (task_context && task_context->kind == XR_XIR_TYPE_TASK)
        child = (SourceExpectedType) {true, task_context->element, context.infer_result, false};
    SourceDirectRequest direct = {target->index, {0}, NULL, child};
    SourceDirectArguments prepared = {0};
    if (!source_direct_arguments(ctx, call, &direct, &prepared)) return false;
    XrXirInstruction op = {XR_XIR_GO, XR_XIR_UNIT, {0}, {0}, target->index, {0}};
    return source_task_type(ctx, prepared.result, &op.type) &&
        source_type_arguments(ctx, call, prepared.substitution.types, prepared.substitution.count, &op) &&
        source_query_reference(ctx, call->as.call_expr.callee, binding, target, XR_XIR_SOURCE_CALL) &&
        source_recipe_group(ctx, op, prepared.values, prepared.count, value);
}
static bool source_task_result_hint(SourceContext *ctx, SourceExpressionPlan *plan,
    SourceExpectedType context, uint32_t depth) {
    if (!context.present) return true;
    if (!plan) return source_fail(ctx, NULL, XR_XIR_BAD_STRUCTURE, "Task result plan is missing");
    if (!source_work(ctx, plan->syntax)) return false;
    if (depth >= 128) return source_fail(ctx, plan->syntax, XR_XIR_BUDGET, "Task result inference depth exhausted");
    switch (plan->syntax->type) {
    case AST_GO_EXPR: case AST_CALL_EXPR: case AST_MATCH_EXPR:
        plan->task_element_context = context;
        return true;
    case AST_GROUPING:
        return !plan->left || source_task_result_hint(ctx, plan->left, context, depth + 1);
    case AST_TERNARY:
        return source_task_result_hint(ctx, plan->left, context, depth + 1) &&
            source_task_result_hint(ctx, plan->right, context, depth + 1);
    default: return true;
    }
}
static bool source_task_expression(SourceContext *ctx, AstNode *node, SourceExpectedType expected,
    SourceExpectedType task_context, SourceValue *value) {
    SourceExpressionPlan *plan = source_plan_collect(ctx, node, expected);
    return plan && source_task_result_hint(ctx, plan, task_context, 0) && source_plan_complete(ctx, plan, value);
}
static bool source_task_await(SourceContext *ctx, AstNode *node, SourceExpectedType context, SourceValue *value) {
    AwaitExprNode *await = &node->as.await_expr;
    if (!await->expr || await->timeout || await->into || await->is_any || await->is_all || await->is_any_success)
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "await requires one Task without aggregation or timeout");
    SourceValue task;
    /* Context is evidence for an ordinary generic call, never a conversion
     * of an existing invariant Task handle to a different element type. */
    if (!source_task_expression(ctx, await->expr, (SourceExpectedType){0}, context, &task)) return false;
    const XrXirTypeNode *type = xr_xir_type_node(&ctx->types, task.type);
    if (!type || type->kind != XR_XIR_TYPE_TASK)
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "await requires an admitted Task value");
    XrXirType element = type->element;
    SourceFunction *body = &ctx->bodies[ctx->function];
    if (!body->block_count && !begin_block(ctx)) return false;
    uint32_t origin = body->count, error = body->block_count;
    if (error == UINT32_MAX) return source_fail(ctx, node, XR_XIR_BUDGET, "await block identity exhausted");
    XrXirInstruction op = {XR_XIR_TASK_AWAIT, XR_XIR_UNIT, {task.id, 0}, {error + 1, error}, 0, {0}};
    if (!source_recipe_append(ctx, op, NULL) || !begin_block(ctx)) return false;
    SourceValue caught;
    if (!source_recipe_append(ctx, (XrXirInstruction) {XR_XIR_INVOKE_ERROR, XR_XIR_ERROR, {0}, {0}, origin, {0}}, &caught))
        return false;
    if (body->error_context) {
        if (!source_error_edge(ctx, caught)) return false;
    } else if (!source_recipe_append(ctx, (XrXirInstruction) {XR_XIR_THROW, XR_XIR_UNIT, {caught.id, 0}, {0}, 0, {0}}, NULL))
        return false;
    if (!begin_block(ctx)) return false;
    if (element == XR_XIR_UNIT) { *value = (SourceValue){UINT32_MAX, XR_XIR_UNIT}; return true; }
    return source_recipe_append(ctx,
        (XrXirInstruction) {XR_XIR_INVOKE_RESULT, element, {0}, {0}, origin, {0}}, value);
}
