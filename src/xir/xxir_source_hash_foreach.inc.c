/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_hash_foreach.inc.c - Snapshot counting loops use ordinary indirect calls
 */
static bool source_hash_callback(SourceContext *ctx, AstNode *node,
    const SourceArrayCallbackShape *shape, SourceArrayCallback *callback) {
    XrXirType parameter = XR_XIR_UNIT;
    SourceExpectedType context = {0};
    SourceValue value = {0};
    SourceCallPlan plan = {SOURCE_CALL_INDIRECT,{0},{0},&parameter,NULL,XR_XIR_UNIT,
        {0},&value,0,0,NULL,&context,0,0};
    SourceCallSyntax syntax;
    if (!source_call_syntax(ctx, node, &syntax) || !source_call_expand(ctx, node, &syntax, &plan, 1)) return false;
    if (syntax.arg_count != 1 || !syntax.expanded)
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "hash callback requires one declared argument");
    if (!source_array_callback_context(ctx, node, syntax.expanded->entries[0].plan.value, shape, &context)) return false;
    parameter = context.type;
    if (!source_call_plan_arguments(ctx, node, &plan, &syntax)) return false;
    callback->value = value;
    return source_array_callback_check(ctx, node, shape, callback);
}
static bool source_hash_for_each(SourceContext *ctx, AstNode *node, const SourceValue *evaluated,
    XrXirType receiver_type, SourceValue *value) {
    CallExprNode *call = &node->as.call_expr; MemberAccessNode *access = &call->callee->as.member_access;
    SourceValue container = {0};
    if (evaluated) container = *evaluated;
    else if (!source_array_receiver(ctx, access->object, NULL, false, &container)) return false;
    if (container.type != receiver_type) return source_fail(ctx, node, XR_XIR_BAD_TYPE, "hash callback receiver type changed");
    const XrXirTypeNode *type = xr_xir_type_node(&ctx->types, receiver_type);
    bool map = type->kind == XR_XIR_TYPE_MAP;
    XrXirType key = type->element, element = type->result;
    SourceArrayCallbackShape shape = {{key, element}, map ? 2u : 1u, map ? 2u : 1u,
        {true, XR_XIR_UNIT, false, false}};
    SourceArrayCallback callback = {0};
    if (!source_hash_callback(ctx, node, &shape, &callback)) return false;
    SourceValue length = {0}, zero = {0};
    if (!source_recipe_record(ctx, (XrXirInstruction){XR_XIR_HASH_LEN, XR_XIR_I64, {container.id}, {0}, 0, {0}}, &length) ||
        !source_recipe_record(ctx, (XrXirInstruction){XR_XIR_CONST_INT, XR_XIR_I64, {0}, {0}, 0, {0}}, &zero)) return false;
    SourceCounter counter = {0};
    if (!source_counter_open(ctx, zero, length, false, &counter)) return false;
    SourceValue arguments[2] = {{0}};
    if (!source_recipe_record(ctx, (XrXirInstruction){XR_XIR_HASH_KEY_AT, key,
        {container.id, counter.index.id}, {0}, 0, {0}}, arguments)) return false;
    if (map && !source_recipe_record(ctx, (XrXirInstruction){XR_XIR_MAP_VALUE_AT, element,
        {container.id, counter.index.id}, {0}, 0, {0}}, arguments + 1)) return false;
    if (!source_recipe_group(ctx, (XrXirInstruction){XR_XIR_CALL_INDIRECT, XR_XIR_UNIT,
        {0}, {0}, callback.value.id, {0}}, arguments, callback.arity, NULL) || !source_counter_close(ctx, &counter)) return false;
    *value = (SourceValue){UINT32_MAX, XR_XIR_UNIT}; return true;
}
