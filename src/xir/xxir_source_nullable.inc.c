/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_nullable.inc.c - Presence tests, force unwrap and null coalescing
 *
 * KEY CONCEPT:
 *   Each form inspects only the outermost Nullable layer; none requires the element to be
 *   comparable, and only the unwrapping forms can fail, with the null-unwrap panic.
 */
static bool source_null_literal(const SourceExpressionPlan *plan) {
    return plan && plan->syntax && plan->syntax->type == AST_LITERAL_NULL;
}
/* `x == null` and `x != null` test presence; the element needs no equality. */
static bool source_null_comparison(const SourceExpressionPlan *plan) {
    return plan && plan->left && plan->right &&
        (source_null_literal(plan->left) != source_null_literal(plan->right));
}
static bool source_null_test(SourceContext *ctx, AstNode *node, SourceValue *value) {
    SourceExpressionPlan *plan = ctx->active_expression;
    bool null_on_left = source_null_literal(plan->left);
    SourceExpressionPlan *subject = null_on_left ? plan->right : plan->left;
    (null_on_left ? plan->left : plan->right)->state = SOURCE_TERM_DISCARDED;
    SourceValue operand;
    if (!source_plan_complete(ctx, subject, &operand)) return false;
    if (!xr_xir_type_is_nullable(&ctx->types, operand.type))
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "comparison with null requires a nullable operand");
    return source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_NULLABLE_IS_SOME, XR_XIR_BOOL, {operand.id, 0}, {0},
        node->type == AST_BINARY_EQ ? 1 : 0, {0}}, value);
}
static bool source_unwrap_value(SourceContext *ctx, AstNode *node, SourceValue nullable, SourceValue *value) {
    if (!xr_xir_type_is_nullable(&ctx->types, nullable.type))
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "unwrap requires a nullable operand");
    return source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_NULLABLE_UNWRAP, xr_xir_nullable_element(&ctx->types, nullable.type),
        {nullable.id, 0}, {0}, 0, {0}}, value);
}
static bool source_force_unwrap(SourceContext *ctx, AstNode *node, SourceValue *value) {
    SourceValue operand;
    return expression(ctx, node->as.unary.operand, &operand) && source_unwrap_value(ctx, node, operand, value);
}
/* `a ?? b`: the element when present, otherwise b; b is evaluated only when a is absent. */
static bool source_coalesce(SourceContext *ctx, AstNode *node, SourceExpectedType expected, SourceValue *value) {
    SourceValue subject, some;
    if (!expression(ctx, node->as.binary.left, &subject)) return false;
    if (!xr_xir_type_is_nullable(&ctx->types, subject.type))
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "?? requires a nullable left operand");
    XrXirType element = xr_xir_nullable_element(&ctx->types, subject.type), result = element;
    AstNode *right = node->as.binary.right;
    if (right->type == AST_LITERAL_NULL ||
        (expected.present && xr_xir_type_is_nullable(&ctx->types, expected.type) &&
         xr_xir_nullable_element(&ctx->types, expected.type) == element)) result = subject.type;
    if (!source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_NULLABLE_IS_SOME, XR_XIR_BOOL, {subject.id, 0}, {0}, 0, {0}}, &some))
        return false;
    SourceFunction *body = &ctx->bodies[ctx->function];
    if (!body->block_count && !begin_block(ctx)) return false;
    uint32_t branch = body->count, yes_block = body->block_count;
    SourceValue yes = subject, no;
    if (!source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_BRANCH, XR_XIR_UNIT, {some.id}, {0}, 0, {0}}, NULL) ||
        !begin_block(ctx)) return false;
    if (result == element && !source_unwrap_value(ctx, node, subject, &yes)) return false;
    uint32_t yes_end = body->current_block.identity, yes_jump = body->count;
    if (!source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_JUMP, XR_XIR_UNIT, {0}, {0}, 0, {0}}, NULL)) return false;
    uint32_t no_block = body->block_count;
    if (!begin_block(ctx)) return false;
    if (right->type == AST_LITERAL_NULL) {
        if (!source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_NULLABLE_NONE, result, {0}, {0}, 0, {0}}, &no)) return false;
    } else {
        if (!source_plan_expression(ctx, right, (SourceExpectedType) {true, result}, &no)) return false;
        if (no.type == element && result != element &&
            !source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_NULLABLE_SOME, result, {no.id, 0}, {0}, 0, {0}}, &no)) return false;
        if (no.type != result)
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "?? default must match the unwrapped element or its nullable context");
    }
    uint32_t no_end = body->current_block.identity, no_jump = body->count;
    if (!source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_JUMP, XR_XIR_UNIT, {0}, {0}, 0, {0}}, NULL)) return false;
    uint32_t join = body->block_count;
    if (!begin_block(ctx)) return false;
    body->recipes[branch].instruction.targets[0] = yes_block; body->recipes[branch].instruction.targets[1] = no_block;
    body->recipes[yes_jump].instruction.targets[0] = join; body->recipes[no_jump].instruction.targets[0] = join;
    SourceValue inputs[] = {{yes_end, XR_XIR_UNIT}, yes, {no_end, XR_XIR_UNIT}, no};
    return source_recipe_group(ctx, (XrXirInstruction) {XR_XIR_PHI, result, {0}, {0}, 0, {0}}, inputs, 4, value);
}
