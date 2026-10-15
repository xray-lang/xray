/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_tuple.inc.c - Ordered evaluation with a non-Unit operand table
 */
static bool source_tuple_index(SourceContext *ctx,AstNode *node,const char *name,uint32_t *field) {
    size_t length=0;uint32_t index=0;
    if (!source_text_length(ctx,node,name,&length)) return false;
    if (!length) return source_fail(ctx,node,XR_XIR_BAD_TYPE,"Tuple field index is empty");
    for (size_t i=0;i<length;++i) {
        if (!source_work(ctx,node)) return false;
        unsigned char digit=(unsigned char)name[i];
        if (digit<'0' || digit>'9' || index>(UINT32_MAX-(digit-'0'))/10)
            return source_fail(ctx,node,XR_XIR_BAD_TYPE,"Tuple field requires a bounded decimal index");
        index=index*10+(digit-'0');
    }
    *field=index;return true;
}
typedef struct SourceTuplePrefix {
    const XrXirCallableParameter *fields;
    uint32_t count;
    bool inferred;
    const SourceExpectedType *contexts;
    SourceExpressionPlan *const *plans;
} SourceTuplePrefix;
static SourceExpectedType source_tuple_field_context(const SourceTuplePrefix *context,uint32_t index) {
    if (context && context->contexts) return context->contexts[index];
    return (SourceExpectedType){context!=NULL,context ? context->fields[index].type : XR_XIR_UNIT,
        false,!context || context->inferred};
}
static bool source_tuple_expanded_literal(SourceContext *ctx,AstNode *node,
    SourceExpectedType expected,SourceValue *value);
static bool source_tuple_literal(SourceContext *ctx,AstNode *node,SourceExpectedType expected,SourceValue *value) {
    const TupleLiteralNode *literal=&node->as.tuple_literal;
    if (literal->count<0 || (literal->count && !literal->elements))
        return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"Tuple literal ordered fields are missing");
    if (!literal->count) {
        if (expected.present && expected.type!=XR_XIR_UNIT)
            return source_fail(ctx,node,XR_XIR_BAD_TYPE,"empty Tuple syntax denotes Unit");
        *value=(SourceValue){UINT32_MAX,XR_XIR_UNIT};return true;
    }
    if (ctx->active_expression->tuple_prefix)
        return source_tuple_expanded_literal(ctx,node,expected,value);
    for (int i=0;i<literal->count;++i) {
        if (!source_work(ctx,literal->elements[i])) return false;
        if (literal->elements[i] && literal->elements[i]->type==AST_SPREAD_EXPR)
            return source_tuple_expanded_literal(ctx,node,expected,value);
    }
    const XrXirTypeNode *context=expected.present ? xr_xir_tuple_signature(&ctx->types,expected.type) : NULL;
    uint32_t count=(uint32_t)literal->count;
    if (expected.present && (!context || context->parameter_count!=count))
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"Tuple literal requires an exact ordered arity context");
    XrXirCallableParameter *fields=source_alloc(ctx,count,sizeof(*fields));
    SourceValue *payloads=source_recipe_storage(ctx,count,sizeof(*payloads));
    if (!fields || !payloads) return false;
    /* Child evaluation can grow the type pool. Keep its field context owned. */
    if (context && !source_copy_bytes(ctx,node,fields,context->parameters,count*sizeof(*fields))) return false;
    uint32_t payload_count=0;
    for (uint32_t i=0;i<count;++i) {
        AstNode *child=literal->elements[i];SourceValue element={0};
        if (!child) return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"Tuple literal entry is missing");
        SourceExpectedType hint={expected.present,fields[i].type,false, expected.inferred};
        if (!source_plan_expression(ctx,child,hint,&element)) return false;
        if (expected.present && element.type!=fields[i].type)
            return source_fail(ctx,node,XR_XIR_BAD_TYPE,"Tuple field does not satisfy its ordered context");
        fields[i]=(XrXirCallableParameter){element.type,0};
        if (element.type!=XR_XIR_UNIT) payloads[payload_count++]=element;
    }
    XrXirType tuple=XR_XIR_UNIT;
    if (!source_intern_type(ctx,(XrXirTypeNode){.kind=XR_XIR_TYPE_TUPLE,
        .parameters=fields,.parameter_count=count},&tuple)) return false;
    return source_recipe_group(ctx,(XrXirInstruction){XR_XIR_TUPLE_NEW,tuple,{0},{0},0,{0}},
        payloads,payload_count,value);
}
static bool source_tuple_spread_value(SourceContext *ctx,AstNode *node,
    const SourceTuplePrefix *context,SourceValue *value) {
    SourceExpressionPlan *plan=source_plan_collect(ctx,node,(SourceExpectedType){0});
    if (!plan) return false;
    SourceExpressionPlan *literal=plan;
    while (literal->syntax->type==AST_GROUPING && literal->left) {
        if (!source_work(ctx,literal->syntax)) return false;
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
    if (context && *count>=context->count)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"Tuple literal requires an exact ordered arity context");
    SourceExpectedType hint=source_tuple_field_context(context,*count);
    SourceConversionRecipe conversion;
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
        const XrXirTypeNode *signature=xr_xir_tuple_signature(&ctx->types,expected.type);
        if (!signature && expected.type!=XR_XIR_UNIT)
            return source_fail(ctx,node,XR_XIR_BAD_TYPE,"Tuple literal requires an exact ordered arity context");
        owned.count=signature ? signature->parameter_count : 0;owned.inferred=expected.inferred;
        XrXirCallableParameter *fields=owned.count ? source_alloc(ctx,owned.count,sizeof(*fields)) : NULL;
        if (owned.count && (!fields || !source_copy_bytes(ctx,node,fields,signature->parameters,owned.count*sizeof(*fields)))) return false;
        owned.fields=fields;context=&owned;
    }
    SourceValue *fields=NULL;uint32_t count=0,capacity=0;
    for (int i=0;i<literal->count;++i) {
        AstNode *child=literal->elements[i];SourceValue element;
        if (!child || !source_work(ctx,child))
            return child ? false : source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"Tuple literal entry is missing");
        if (child->type!=AST_SPREAD_EXPR) {
            if (count>=65536) return source_fail(ctx,child,XR_XIR_BUDGET,"Tuple expanded arity exhausted");
            if (context && count>=context->count)
                return source_fail(ctx,child,XR_XIR_BAD_TYPE,"Tuple literal requires an exact ordered arity context");
            SourceExpectedType hint=source_tuple_field_context(context,count);
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
        SourceTuplePrefix tail={context && context->fields ? context->fields+count : NULL,
            context ? context->count-count : 0,context && context->inferred,
            context && context->contexts ? context->contexts+count : NULL,
            context && context->plans ? context->plans+count : NULL};
        if (!source_tuple_spread_value(ctx,child,context ? &tail : NULL,&element)) return false;
        if (element.type==XR_XIR_UNIT) continue;
        const XrXirTypeNode *signature=xr_xir_tuple_signature(&ctx->types,element.type);
        if (!signature) return source_fail(ctx,child,XR_XIR_BAD_TYPE,"Tuple literal spread requires a statically known Tuple");
        uint32_t arity=signature->parameter_count;
        XrXirCallableParameter *types=source_recipe_storage(ctx,arity,sizeof(*types));
        if (!types || !source_copy_bytes(ctx,child,types,signature->parameters,arity*sizeof(*types))) return false;
        for (uint32_t f=0;f<arity;++f) {
            SourceValue field;
            if (!source_work(ctx,child) ||
                !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_TUPLE_FIELD,types[f].type,
                    {element.id,0},{0},f,{0}},&field) ||
                !source_tuple_append(ctx,child,context,field,&fields,&count,&capacity)) return false;
        }
    }
    if (exact && count!=context->count)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"Tuple literal requires an exact ordered arity context");
    if (!count) { *value=(SourceValue){UINT32_MAX,XR_XIR_UNIT};return true; }
    XrXirCallableParameter *types=source_alloc(ctx,count,sizeof(*types));
    SourceValue *payloads=source_recipe_storage(ctx,count,sizeof(*payloads));
    if (!types || !payloads) return false;
    uint32_t payload_count=0;
    for (uint32_t f=0;f<count;++f) {
        if (!source_work(ctx,node)) return false;
        types[f]=(XrXirCallableParameter){fields[f].type,0};
        if (fields[f].type!=XR_XIR_UNIT) payloads[payload_count++]=fields[f];
    }
    XrXirType tuple;
    if (!source_intern_type(ctx,(XrXirTypeNode){.kind=XR_XIR_TYPE_TUPLE,
            .parameters=types,.parameter_count=count},&tuple)) return false;
    return source_recipe_group(ctx,(XrXirInstruction){XR_XIR_TUPLE_NEW,tuple,{0},{0},0,{0}},payloads,payload_count,value);
}
static bool source_tuple_field(SourceContext *ctx,AstNode *node,SourceValue receiver,SourceValue *value) {
    uint32_t field=0;
    if (!source_tuple_index(ctx,node,node->as.member_access.name,&field)) return false;
    const XrXirTypeNode *tuple=xr_xir_tuple_signature(&ctx->types,receiver.type);
    if (!tuple || field>=tuple->parameter_count)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"Tuple field index exceeds its ordered arity");
    return source_recipe_record(ctx,(XrXirInstruction){XR_XIR_TUPLE_FIELD,tuple->parameters[field].type,
        {receiver.id,0},{0},field,{0}},value);
}
