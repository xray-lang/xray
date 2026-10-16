/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_iterator_call.inc.c - Governed pull calls with owned successors
 */
static bool source_iterator_call(SourceContext *ctx,AstNode *node,SourceValue receiver,SourceValue *value) {
    CallExprNode *call=&node->as.call_expr;
    if(call->arg_count || call->type_arg_count)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"Iterator pull accepts no value or type arguments");
    const XrNativeMemberDeclaration *member=source_iterator_member(ctx,call->callee,call->callee->as.member_access.name);
    if(!member || !source_iterator_member_reference(ctx,call->callee,member)) return false;
    XrXirType element=member->operation==XR_NATIVE_OPERATION_ITERATOR_HAS_NEXT ? XR_XIR_BOOL :
        xr_xir_iterator_element(&ctx->types,receiver.type);
    SourceFunction *body = &ctx->bodies[ctx->function];
    if (!body->block_count && !begin_block(ctx)) return false;
    uint32_t origin = body->count, error = body->block_count;
    if (error == UINT32_MAX) return source_fail(ctx, node, XR_XIR_BUDGET, "Iterator block identity exhausted");
    XrXirInstruction op = {member->operation==XR_NATIVE_OPERATION_ITERATOR_HAS_NEXT ? XR_XIR_ITERATOR_HAS_NEXT : XR_XIR_ITERATOR_NEXT,
        XR_XIR_UNIT, {receiver.id, 0}, {error + 1, error}, 0, {0}};
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
