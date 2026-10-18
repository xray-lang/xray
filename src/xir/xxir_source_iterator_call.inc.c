/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_iterator_call.inc.c - Governed pull calls with owned successors
 */
static bool source_iterator_consume(SourceContext *ctx,AstNode *node,SourceValue receiver,
    SourceValue index,XrXirOp operation,SourceValue *value) {
    XrXirType element=operation==XR_XIR_ITERATOR_HAS_NEXT ? XR_XIR_BOOL :
        operation==XR_XIR_ITERATOR_CLOSE ? XR_XIR_UNIT : xr_xir_iterator_element(&ctx->types,receiver.type);
    SourceFunction *body = &ctx->bodies[ctx->function];
    if (!body->block_count && !begin_block(ctx)) return false;
    uint32_t origin = body->count, error = body->block_count;
    if (error == UINT32_MAX) return source_fail(ctx, node, XR_XIR_BUDGET, "Iterator block identity exhausted");
    XrXirInstruction op = {operation,XR_XIR_UNIT,
        {receiver.id, operation==XR_XIR_ITERATOR_NTH ? index.id : 0}, {error + 1, error}, 0, {0}};
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
static bool source_iterator_call(SourceContext *ctx,AstNode *node,SourceValue receiver,SourceValue *value) {
    CallExprNode *call=&node->as.call_expr;
    if(call->type_arg_count || call->default_arg_count)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"Iterator pull requires its declared ordinary arguments");
    const XrNativeMemberDeclaration *member=source_iterator_member(ctx,call->callee,call->callee->as.member_access.name);
    if(!member || !source_iterator_member_reference(ctx,call->callee,member)) return false;
    bool nth=member->operation==XR_NATIVE_OPERATION_ITERATOR_NTH;
    if(call->arg_count!=(nth ? 1 : 0) || (nth && (!call->arguments ||
        (call->arg_accesses && call->arg_accesses[0]!=XR_CALL_ARG_PLAIN))))
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"Iterator pull argument shape differs from its declaration");
    SourceValue index={0};
    if(nth && !source_plan_expression(ctx,call->arguments[0],
        (SourceExpectedType){true,XR_XIR_I64,false,false},&index)) return false;
    if(nth && index.type!=XR_XIR_I64)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"Iterator nth index must be exact i64");
    XrXirOp operation=member->operation==XR_NATIVE_OPERATION_ITERATOR_HAS_NEXT ? XR_XIR_ITERATOR_HAS_NEXT :
        nth ? XR_XIR_ITERATOR_NTH : XR_XIR_ITERATOR_NEXT;
    return source_iterator_consume(ctx,node,receiver,index,operation,value);
}
