/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_tuple.inc.c - Ordered evaluation with a non-Unit operand table
 */
static bool source_tuple_index(SourceContext *ctx,AstNode *node,const char *name,uint32_t *field) {
    size_t length=0;
    if (!source_text_length(ctx,node,name,&length)) return false;
    const char *reason=NULL;XrXirStatus status=xr_hir_tuple_index(name,length,ctx->request,field,&reason);
    return status==XR_XIR_OK||source_fail(ctx,node,status,reason?reason:"Tuple index receive failed");
}
typedef struct SourceTuplePrefix {
    XrHirTupleContext context;
    uint32_t offset,count;
    SourceExpressionPlan *const *plans;
} SourceTuplePrefix;
static bool source_tuple_prefix(SourceContext *ctx,const SourceExpectedType *fields,uint32_t count,
    SourceExpressionPlan *const *plans,SourceTuplePrefix *out) {
    XrHirTupleExpected *received=count?source_alloc(ctx,count,sizeof(*received)):NULL;
    if(count&&!received)return false;
    for(uint32_t p=0;p<count;++p) {
        if(ctx->diagnostic.status!=XR_XIR_OK)return false;
        received[p]=(XrHirTupleExpected){fields[p].type,fields[p].present,fields[p].infer_result,fields[p].inferred};
    }
    XrHirTupleContextInput input={.types=&ctx->types,.fields=received,.count=count,.kind=XR_HIR_TUPLE_PREFIX};
    XrHirTupleContext context={0};const char *reason=NULL;
    XrXirStatus status=xr_hir_body_tuple_context(ctx->typed_body,&input,ctx->request,&context,&reason);
    if(status!=XR_XIR_OK)return source_fail(ctx,NULL,status,reason?reason:"Tuple prefix receive failed");
    *out=(SourceTuplePrefix){context,0,count,plans};return true;
}
static bool source_tuple_context(SourceContext *ctx,AstNode *node,SourceExpectedType expected,
    uint32_t count,SourceTuplePrefix *out) {
    XrHirTupleContextInput input={.types=&ctx->types,.type=expected.type,.count=count,
        .kind=XR_HIR_TUPLE_EXACT,.from_type=true,.inferred=expected.inferred};
    XrHirTupleContext context={0};const char *reason=NULL;
    XrXirStatus status=xr_hir_body_tuple_context(ctx->typed_body,&input,ctx->request,&context,&reason);
    if(status!=XR_XIR_OK)return source_fail(ctx,node,status,status==XR_XIR_BAD_TYPE?
        "Tuple literal requires an exact ordered arity context":reason?reason:"Tuple context receive failed");
    uint32_t size=0;if(!xr_hir_body_tuple_context_size(ctx->typed_body,context,&size))return false;
    *out=(SourceTuplePrefix){context,0,size,NULL};return true;
}
static SourceExpectedType source_tuple_field_context(SourceContext *ctx,AstNode *node,const SourceTuplePrefix *context,uint32_t index) {
    if(!context)return (SourceExpectedType){.inferred=true};
    XrHirTupleExpected expected={0};
    if(index>=context->count||!xr_hir_body_tuple_expected(ctx->typed_body,context->context,context->offset+index,&expected)) {
        source_fail(ctx,node,XR_XIR_BAD_TYPE,"Tuple literal requires an exact ordered arity context");return (SourceExpectedType){0};
    }
    return (SourceExpectedType){expected.present,expected.type,expected.infer_result,expected.inferred};
}
static bool source_tuple_shape(SourceContext *ctx,AstNode *node,XrXirType type,XrHirTupleShape *shape,XrHirTupleInfo *info) {
    const char *reason=NULL;XrXirStatus status=xr_hir_body_tuple_receive(ctx->typed_body,&ctx->types,type,ctx->request,shape,&reason);
    if(status!=XR_XIR_OK)return source_fail(ctx,node,status,reason?reason:"Tuple requires a statically known ordered shape");
    return xr_hir_body_tuple_info(ctx->typed_body,*shape,info)||source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"Tuple shape has no owner");
}
static bool source_tuple_construct(SourceContext *ctx,AstNode *node,XrXirType tuple,
    const SourceValue *fields,uint32_t count,const SourceTuplePrefix *context,SourceValue *value) {
    XrHirTupleShape shape={0};XrHirTupleInfo info={0};
    if(!source_tuple_shape(ctx,node,tuple,&shape,&info))return false;
    XrHirTupleValue *values=count?source_alloc(ctx,count,sizeof(*values)):NULL;
    if(count&&!values)return false;
    for(uint32_t p=0;p<count;++p){if(ctx->diagnostic.status!=XR_XIR_OK)return false;values[p]=(XrHirTupleValue){fields[p].type,fields[p].id};}
    XrHirBodyScope scope={0};if(!source_hir_current_scope(ctx,&scope))return false;
    XrHirConversionContext conversion={.compile=&ctx->compile,.types=&ctx->types,.control=ctx->request,
        .intern_owner=ctx,.intern=source_conversion_intern};
    XrHirTupleConstructInput input={.types=&ctx->types,.scope=scope,.origin=source_hir_origin(ctx,node,0),.shape=shape,
        .context=context?context->context:(XrHirTupleContext){0},.values=values,.conversion=&conversion,
        .count=count,.context_offset=context?context->offset:0};
    XrHirTupleAction action;const char *reason=NULL;
    XrXirStatus status=xr_hir_body_tuple_construct(ctx->typed_body,&input,ctx->request,&action,&reason);
    if(status!=XR_XIR_OK)return source_fail(ctx,node,status,reason?reason:"Tuple construction receive failed");
    SourceValue *payloads=action.tuple.physical_count?source_recipe_storage(ctx,action.tuple.physical_count,sizeof(*payloads)):NULL;
    if(action.tuple.physical_count&&!payloads)return false;
    for(uint32_t p=0;p<action.tuple.physical_count;++p){if(ctx->diagnostic.status!=XR_XIR_OK)return false;payloads[p]=fields[action.tuple.physical_fields[p]];}
    uint32_t before=ctx->bodies[ctx->function].count;
    if(action.operation==XR_XIR_INVALID)*value=(SourceValue){.id=UINT32_MAX,.type=action.type,.place={0}};
    else if(!source_recipe_group(ctx,(XrXirInstruction){action.operation,action.type,{0},{0},0,{0}},payloads,action.tuple.physical_count,value))return false;
    return source_hir_binding_emitted(ctx,&(XrHirBindingAction){.use=action.use},value,before);
}
static bool source_tuple_projection(SourceContext *ctx,AstNode *node,SourceValue receiver,uint32_t field,SourceValue *value) {
    XrHirTupleShape shape={0};XrHirTupleInfo info={0};XrHirBodyScope scope={0};
    if(!source_tuple_shape(ctx,node,receiver.type,&shape,&info)||!source_hir_current_scope(ctx,&scope))return false;
    XrHirTupleFieldInput input={.types=&ctx->types,.scope=scope,.origin=source_hir_origin(ctx,node,0),
        .shape=shape,.receiver_type=receiver.type,.receiver=receiver.id,.field=field};
    XrHirTupleAction action;const char *reason=NULL;
    XrXirStatus status=xr_hir_body_tuple_field(ctx->typed_body,&input,ctx->request,&action,&reason);
    if(status!=XR_XIR_OK)return source_fail(ctx,node,status,reason?reason:"Tuple projection receive failed");
    uint32_t before=ctx->bodies[ctx->function].count;
    return source_recipe_record(ctx,(XrXirInstruction){action.operation,action.type,{receiver.id,0},{0},action.field,{0}},value)&&
        source_hir_binding_emitted(ctx,&(XrHirBindingAction){.use=action.use},value,before);
}
static bool source_tuple_expanded_literal(SourceContext *ctx,AstNode *node,
    SourceExpectedType expected,SourceValue *value);
static bool source_tuple_literal(SourceContext *ctx,AstNode *node,SourceExpectedType expected,SourceValue *value) {
    const TupleLiteralNode *literal=&node->as.tuple_literal;
    if (literal->count<0 || (literal->count && !literal->elements))
        return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"Tuple literal ordered fields are missing");
    if (!literal->count) {
        SourceTuplePrefix context={0};
        if(expected.present) {
            XrHirTupleContextInput input={.types=&ctx->types,.type=expected.type,.count=0,.kind=XR_HIR_TUPLE_EXACT,.from_type=true};
            XrXirStatus status=xr_hir_body_tuple_context(ctx->typed_body,&input,ctx->request,&context.context,NULL);
            if(status!=XR_XIR_OK)return source_fail(ctx,node,status,"empty Tuple syntax denotes Unit");
        }
        return source_tuple_construct(ctx,node,XR_XIR_UNIT,NULL,0,expected.present?&context:NULL,value);
    }
    if (ctx->active_expression->tuple_prefix)
        return source_tuple_expanded_literal(ctx,node,expected,value);
    for (int i=0;i<literal->count;++i) {
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
        if (literal->elements[i] && literal->elements[i]->type==AST_SPREAD_EXPR)
            return source_tuple_expanded_literal(ctx,node,expected,value);
    }
    uint32_t count=(uint32_t)literal->count;
    SourceTuplePrefix context={0};if(expected.present&&!source_tuple_context(ctx,node,expected,count,&context))return false;
    XrXirCallableParameter *fields=source_alloc(ctx,count,sizeof(*fields));
    SourceValue *values=source_alloc(ctx,count,sizeof(*values));
    if (!fields || !values) return false;
    for (uint32_t i=0;i<count;++i) {
        AstNode *child=literal->elements[i];SourceValue element={0};
        if (!child) return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"Tuple literal entry is missing");
        SourceExpectedType hint=source_tuple_field_context(ctx,node,expected.present?&context:NULL,i);
        if (!source_plan_expression(ctx,child,hint,&element)) return false;
        fields[i]=(XrXirCallableParameter){element.type,0};
        values[i]=element;
    }
    XrXirType tuple=XR_XIR_UNIT;
    return source_hir_tuple_type(ctx,fields,count,&tuple)&&
        source_tuple_construct(ctx,node,tuple,values,count,expected.present?&context:NULL,value);
}
static bool source_tuple_spread_value(SourceContext *ctx,AstNode *node,
    const SourceTuplePrefix *context,SourceValue *value) {
    SourceExpressionPlan *plan=source_plan_collect(ctx,node,(SourceExpectedType){0});
    if (!plan) return false;
    SourceExpressionPlan *literal=plan;
    while (literal->syntax->type==AST_GROUPING && literal->left) {
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
        literal=literal->left;
    }
    /* A literal consumes its own statically expanded prefix. The enclosing
     * literal checks total arity; no speculative value evaluation is needed. */
    if (context && literal->syntax->type==AST_TUPLE_LITERAL) {
        SourceTuplePrefix *owned=source_recipe_storage(ctx,1,sizeof(*owned));
        if (!owned) return false;
        *owned=*context;literal->tuple_prefix=owned;
    }
    return source_plan_complete(ctx,plan,value);
}
static bool source_tuple_append(SourceContext *ctx,AstNode *node,const SourceTuplePrefix *context,
    SourceValue element,SourceValue **fields,uint32_t *count,uint32_t *capacity) {
    if (*count>=65536) return source_fail(ctx,node,XR_XIR_BUDGET,"Tuple expanded arity exhausted");
    SourceExpectedType hint=source_tuple_field_context(ctx,node,context,*count);
    if(ctx->diagnostic.status!=XR_XIR_OK)return false;
    XrHirConversion conversion;
    if (!source_conversion_plan(ctx,node,element.type,hint,&conversion) ||
        !source_conversion_emit(ctx,&conversion,&element)) return false;
    if ((!hint.present || hint.inferred || hint.infer_result) && !source_root_join_record(ctx,&element)) return false;
    SourceValue *grown=source_query_append(ctx,*fields,count,capacity,sizeof(**fields));
    if (!grown) return false;
    *fields=grown;grown[*count-1]=element;return true;
}
static bool source_tuple_expanded_literal(SourceContext *ctx,AstNode *node,
    SourceExpectedType expected,SourceValue *value) {
    const TupleLiteralNode *literal=&node->as.tuple_literal;
    const SourceTuplePrefix *context=ctx->active_expression->tuple_prefix;
    bool exact=context==NULL && expected.present;
    SourceTuplePrefix owned={0};
    if (exact) {
        if(!source_tuple_context(ctx,node,expected,UINT32_MAX,&owned))return false;context=&owned;
    }
    SourceValue *fields=NULL;uint32_t count=0,capacity=0;
    for (int i=0;i<literal->count;++i) {
        AstNode *child=literal->elements[i];SourceValue element;
        if (!child || !((ctx)->diagnostic.status == XR_XIR_OK))
            return child ? false : source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"Tuple literal entry is missing");
        if (child->type!=AST_SPREAD_EXPR) {
            if (count>=65536) return source_fail(ctx,child,XR_XIR_BUDGET,"Tuple expanded arity exhausted");
            SourceExpectedType hint=source_tuple_field_context(ctx,child,context,count);
            if(ctx->diagnostic.status!=XR_XIR_OK)return false;
            SourceExpressionPlan *prepared=context && context->plans ? context->plans[count] : NULL;
            if (prepared) {
                if (prepared->syntax!=child)
                    return source_fail(ctx,child,XR_XIR_BAD_STRUCTURE,"Tuple field plan no longer owns its original syntax");
                prepared->expected=hint;
                if (!source_plan_complete(ctx,prepared,&element)) return false;
            } else if (!source_plan_expression(ctx,child,hint,&element)) return false;
            if ((!hint.present || hint.inferred || hint.infer_result) && !source_root_join_record(ctx,&element)) return false;
            /* The ordinary expression plan already consumed this conversion. */
            SourceValue *grown=source_query_append(ctx,fields,&count,&capacity,sizeof(*fields));
            if (!grown) return false;
            fields=grown;fields[count-1]=element;continue;
        }
        child=child->as.spread_expr.expr;
        if (!child) return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"Tuple spread source is missing");
        SourceTuplePrefix tail={context?context->context:(XrHirTupleContext){0},context?context->offset+count:0,
            context?context->count-count:0,context&&context->plans?context->plans+count:NULL};
        if(context) {
            XrHirTupleContext sliced={0};XrXirStatus status=xr_hir_body_tuple_context_slice(ctx->typed_body,
                context->context,tail.offset,tail.count,ctx->request,&sliced);
            if(status!=XR_XIR_OK)return source_fail(ctx,child,status,"Tuple prefix slice receive failed");
            tail.context=sliced;tail.offset=0;
        }
        if (!source_tuple_spread_value(ctx,child,context ? &tail : NULL,&element)) return false;
        if (element.type==XR_XIR_UNIT) continue;
        XrHirTupleShape shape={0};XrHirTupleInfo info={0};
        XrXirStatus status=xr_hir_body_tuple_receive(ctx->typed_body,&ctx->types,element.type,ctx->request,&shape,NULL);
        if(status!=XR_XIR_OK)return source_fail(ctx,child,status,status==XR_XIR_BAD_TYPE?
            "Tuple literal spread requires a statically known Tuple":"Tuple spread receive failed");
        if(!xr_hir_body_tuple_info(ctx->typed_body,shape,&info))return false;
        for (uint32_t f=0;f<info.count;++f) {
            SourceValue field;
            if (!((ctx)->diagnostic.status == XR_XIR_OK) ||
                !source_tuple_projection(ctx,child,element,f,&field) ||
                !source_tuple_append(ctx,child,context,field,&fields,&count,&capacity)) return false;
        }
    }
    if (!count)return source_tuple_construct(ctx,node,XR_XIR_UNIT,NULL,0,context,value);
    XrXirCallableParameter *types=source_alloc(ctx,count,sizeof(*types));
    if (!types) return false;
    for (uint32_t f=0;f<count;++f) {
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
        types[f]=(XrXirCallableParameter){fields[f].type,0};
    }
    XrXirType tuple;
    return source_hir_tuple_type(ctx,types,count,&tuple)&&source_tuple_construct(ctx,node,tuple,fields,count,context,value);
}
static bool source_tuple_field(SourceContext *ctx,AstNode *node,SourceValue receiver,SourceValue *value) {
    uint32_t field=0;
    if (!source_tuple_index(ctx,node,node->as.member_access.name,&field)) return false;
    return source_tuple_projection(ctx,node,receiver,field,value);
}
