/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_iterable.inc.c - Implicit iterator calls retain ordinary method authority
 */
/* Ref iteration borrows the original mutable path, including its selectors,
 * rather than copying a receiver and then trying to recover write authority. */
static bool source_iteration_receiver(SourceContext *ctx,AstNode *node,SourceValue *receiver) {
    AstNode *collection=node->as.for_in_stmt.collection;
    SourceName *binding=source_ref_binding(ctx,collection);
    if(binding) {
        XrXirType type;
        if(!source_path_type(ctx,collection,&type)) return false;
        SourceName *method=source_method_find(ctx,type,"iterator");
        if(method) {
            const XrXirFunctionIdentity *identity=source_method_identity(ctx,node,method);
            if(!identity) return false;
            const XrXirFunction *function=&ctx->functions[method->index];
            if(identity->method_kind==XR_XIR_READ_METHOD &&
                xr_xir_type_is_cell(&ctx->types,function->parameters[0]))
                return source_reference_place(ctx,collection,receiver);
        }
    }
    return expression(ctx,collection,receiver);
}

/* The receiver has already been evaluated. The implicit call reuses ordinary
 * visibility, generic proof, argument modes, effects and query references. */
static bool source_iteration_factory(SourceContext *ctx,AstNode *node,
    SourceValue receiver,SourceValue *iterator) {
    XrNameSpan keyword=node->as.for_in_stmt.keyword_span;
    if(keyword.line<=0 || keyword.column<=0 || keyword.column>INT_MAX-3)
        return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"implicit iteration requires its actual for token span");
    AstNode *syntax=source_alloc(ctx,2,sizeof(*syntax));
    if(!syntax) return false;
    AstNode *call=&syntax[0],*member=&syntax[1];
    call->type=AST_CALL_EXPR;call->line=keyword.line;call->column=keyword.column;
    /* The implicit call selects the for keyword, preserving every explicit
     * receiver token for its own declaration and constructor references. */
    member->type=AST_MEMBER_ACCESS;member->line=keyword.line;member->column=keyword.column;
    member->end_line=keyword.line;member->end_column=keyword.column+3;
    member->as.member_access.object=node->as.for_in_stmt.collection;
    member->as.member_access.name="iterator";call->as.call_expr.callee=member;
    bool parameter=(uint32_t)receiver.type>=XR_XIR_TYPE_PARAMETER_BASE &&
        (uint32_t)receiver.type<XR_XIR_TYPE_PARAMETER_LIMIT;
    if(parameter) {
        if(!source_requirement_call(ctx,call,receiver,(SourceExpectedType){0},iterator)) return false;
    } else {
        XrXirType receiver_type=xr_xir_type_is_cell(&ctx->types,receiver.type) ?
            xr_xir_cell_element(&ctx->types,receiver.type) : receiver.type;
        SourceName *method=source_method_find(ctx,receiver_type,"iterator");
        if(!method) return source_fail(ctx,node,XR_XIR_BAD_TYPE,"for-in requires an iterator method returning Iterator");
        if(!source_method_call(ctx,call,receiver,method,(SourceExpectedType){0},iterator)) return false;
        if(xr_xir_type_is_cell(&ctx->types,receiver.type) &&
            !source_fact_push(ctx,source_ref_binding(ctx,node->as.for_in_stmt.collection),0)) return false;
    }
    const XrXirTypeNode *type=xr_xir_type_node(&ctx->types,iterator->type);
    return (type && type->kind==XR_XIR_TYPE_ITERATOR) ||
        source_fail(ctx,node,XR_XIR_BAD_TYPE,"iterator method must return the canonical Iterator type");
}
