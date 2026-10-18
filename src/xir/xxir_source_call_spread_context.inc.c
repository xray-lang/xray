/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_call_spread_context.inc.c - Checked context hints without value evaluation
 *
 * Hints supply literal contexts only. The original expression plan still
 * validates the complete Tuple and every projected argument independently.
 */
static bool source_call_spread_hint(SourceContext *ctx,AstNode *node,uint32_t depth,SourceExpectedType *out) {
    *out=(SourceExpectedType){0};
    if(!node || !source_work(ctx,node)) return false;
    if(depth>=128) return source_fail(ctx,node,XR_XIR_BUDGET,"call spread context depth exhausted");
    if(node->type==AST_GROUPING) return source_call_spread_hint(ctx,node->as.grouping,depth+1,out);
    if(!source_ground_type(ctx,node,depth,out) || out->present) return ctx->diagnostic.status==XR_XIR_OK;
    SourceInteger integer={0};SourceDecimal decimal={0};
    if(!source_direct_integer(ctx,node,&integer) || !source_direct_decimal(ctx,node,&decimal)) return false;
    if(integer.present || decimal.node) {
        SourceNumericRecipe numeric={0};
        SourceNumericRequest request={integer.present?&integer:NULL,decimal.node?&decimal:NULL,{0},true};
        if(!source_numeric_plan(ctx,node,&request,&numeric)) return false;
        *out=(SourceExpectedType){true,numeric.type,false,false};return true;
    }
    if(node->type==AST_LITERAL_STRING || node->type==AST_LITERAL_RUNE || node->type==AST_LITERAL_TRUE || node->type==AST_LITERAL_FALSE) {
        *out=(SourceExpectedType){true,node->type==AST_LITERAL_STRING?XR_XIR_STRING:
            node->type==AST_LITERAL_RUNE?XR_XIR_RUNE:XR_XIR_BOOL,false,false};return true;
    }
    if(node->type>=AST_BINARY_ADD && node->type<=AST_BINARY_GE) {
        SourceExpectedType left={0},right={0};
        if(!source_call_spread_hint(ctx,node->as.binary.left,depth+1,&left) ||
            !source_call_spread_hint(ctx,node->as.binary.right,depth+1,&right)) return false;
        if(left.present && right.present && left.type==right.type &&
            (xr_xir_type_is_integer(left.type) || xr_xir_float_bits(left.type) ||
             (node->type==AST_BINARY_ADD && left.type==XR_XIR_STRING)))
            *out=(SourceExpectedType){true,node->type>=AST_BINARY_EQ?XR_XIR_BOOL:left.type,false,false};
        return true;
    }
    if(node->type==AST_ARRAY_LITERAL) {
        const ArrayLiteralNode *array=&node->as.array_literal;
        if(array->is_repeat || array->count<=0 || !array->elements) return true;
        XrXirType element=XR_XIR_UNIT;
        for(int i=0;i<array->count;++i) {
            SourceExpectedType child={0};
            if(!source_call_spread_hint(ctx,array->elements[i],depth+1,&child)) return false;
            if(!child.present || (i && child.type!=element)) return true;
            element=child.type;
        }
        if(!source_intern_type(ctx,(XrXirTypeNode){.kind=XR_XIR_TYPE_ARRAY,.element=element},&out->type)) return false;
        out->present=true;return true;
    }
    if(node->type!=AST_TUPLE_LITERAL) return true;
    uint32_t count=0;if(!source_call_spread_shape(ctx,node,depth+1,&count)) return false;
    /* Interned type nodes retain their fields beyond the expression region. */
    XrXirCallableParameter *fields=count?source_alloc(ctx,count,sizeof(*fields)):NULL;
    if(count && !fields) return false;
    uint32_t at=0;
    for(int i=0;i<node->as.tuple_literal.count;++i) {
        AstNode *child=node->as.tuple_literal.elements[i];SourceExpectedType hint={0};
        bool spread=child->type==AST_SPREAD_EXPR;
        if(!source_call_spread_hint(ctx,spread?child->as.spread_expr.expr:child,depth+1,&hint)) return false;
        if(!hint.present) return true;
        if(!spread) {fields[at++]=(XrXirCallableParameter){hint.type,0};continue;}
        if(hint.type==XR_XIR_UNIT) continue;
        const XrXirTypeNode *tuple=xr_xir_tuple_signature(&ctx->types,hint.type);
        if(!tuple || tuple->parameter_count>count-at) return true;
        for(uint32_t f=0;f<tuple->parameter_count;++f) {
            if(!source_work(ctx,child)) return false;
            fields[at++]=tuple->parameters[f];
        }
    }
    if(at!=count) return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"call spread hint arity changed");
    if(!source_intern_type(ctx,(XrXirTypeNode){.kind=XR_XIR_TYPE_TUPLE,.parameters=fields,.parameter_count=count},&out->type)) return false;
    out->present=true;return true;
}
static bool source_call_expected(SourceContext *ctx,AstNode *node,const SourceCallPlan *plan,
    XrXirInferenceState *state,uint32_t parameter,SourceExpectedType *out) {
    *out=(SourceExpectedType){0};
    XrXirType formal=plan->family==SOURCE_CALL_REQUIREMENT?
        plan->requirement_parameters[parameter].type:plan->function_parameters[parameter];
    SourceSubstitution substitution=plan->substitution;
    if(state) {
        XrXirInferenceKnown known={0};
        XrXirStatus status=xr_xir_compile_inference_expected_known(state,&ctx->types,formal,&known);
        if(status!=XR_XIR_OK) return source_fail(ctx,node,status,"direct call inference context is invalid");
        if(!known.known) return true;
        substitution=(SourceSubstitution){known.arguments,known.argument_count};
    }
    out->present=true;return source_substitute(ctx,&substitution,formal,0,&out->type);
}
static bool source_call_spread_events(SourceContext *ctx,AstNode *node,const SourceCallSyntax *call,
    const SourceCallPlan *formal,XrXirInferenceState *state,uint32_t position) {
    if(!call->expanded) return true;
    while(call->expanded->next_spread<call->expanded->spread_count) {
        SourceCallSpread *spread=&call->expanded->spreads[call->expanded->next_spread];
        if(!source_work(ctx,node)) return false;
        if(spread->first>position) break;
        if(spread->first!=position) return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"call spread event order changed");
        ++call->expanded->next_spread;
        if(!spread->count) {
            SourceValue empty;if(!source_plan_complete(ctx,spread->source,&empty)) return false;
            const XrXirTypeNode *tuple=xr_xir_tuple_signature(&ctx->types,empty.type);
            if(empty.type!=XR_XIR_UNIT && (!tuple || tuple->parameter_count))
                return source_fail(ctx,node,XR_XIR_BAD_TYPE,"call spread arity differs from its static shape");
            continue;
        }
        if(spread->source->type_ready) continue;
        /* The context joins the Source type pool, whose owner outlives recipes. */
        XrXirCallableParameter *fields=source_alloc(ctx,spread->count,sizeof(*fields));
        if(!fields) return false;
        bool complete=true;
        for(uint32_t f=0;f<spread->count;++f) {
            SourceExpectedType expected={0};uint32_t a=spread->first+f;
            if(!source_call_expected(ctx,node,formal,state,a+formal->parameter_offset,&expected)) return false;
            SourceExpressionPlan *projection=call->expanded->entries[a].plan.value;
            if(!expected.present && projection->type_ready) expected=(SourceExpectedType){true,projection->ground_type,false,false};
            if(!expected.present && spread->fields[f] && !source_call_spread_hint(ctx,spread->fields[f],0,&expected)) return false;
            if(!expected.present) {complete=false;break;}
            fields[f]=(XrXirCallableParameter){expected.type,0};
        }
        if(complete) {
            XrXirType tuple=XR_XIR_UNIT;
            if(!source_intern_type(ctx,(XrXirTypeNode){.kind=XR_XIR_TYPE_TUPLE,.parameters=fields,.parameter_count=spread->count},&tuple)) return false;
            spread->source->expected=(SourceExpectedType){true,tuple,false,false};
        }
    }
    return true;
}
static bool source_call_projection_complete(SourceContext *ctx,const SourceCallEntry *entry,
    SourceExpectedType expected,SourceValue *value) {
    SourceExpressionPlan *plan=entry->plan.value;
    if(!entry->spread || plan->owner!=ctx->function || ctx->bodies[ctx->function].region_sealed ||
        plan->state!=SOURCE_TERM_UNRESOLVED)
        return source_fail(ctx,entry->syntax,XR_XIR_BAD_STRUCTURE,"call projection has no active owner");
    if(ctx->depth>=128) return source_fail(ctx,entry->syntax,XR_XIR_BUDGET,"source expression depth exhausted");
    SourceExpressionPlan *parent=ctx->active_expression;
    SourceFunction *body=&ctx->bodies[ctx->function];
    plan->entry=body->block_count?body->current_block:(SourceBlockReference){ctx->function,0};
    plan->state=SOURCE_TERM_CHECKING;ctx->active_expression=plan;++ctx->depth;
    SourceValue tuple;bool ok=source_plan_complete(ctx,entry->spread->source,&tuple);
    if(ok) {
        const XrXirTypeNode *type=xr_xir_tuple_signature(&ctx->types,tuple.type);
        if(!type || type->parameter_count!=entry->spread->count || entry->field>=type->parameter_count)
            ok=source_fail(ctx,entry->syntax,XR_XIR_BAD_TYPE,"call spread arity differs from its static shape");
        else {
            XrXirType field=type->parameters[entry->field].type;
            ok=source_recipe_record(ctx,(XrXirInstruction){XR_XIR_TUPLE_FIELD,field,{tuple.id,0},{0},entry->field,{0}},value);
        }
    }
    SourceConversionRecipe conversion={0};
    ok=ok && source_conversion_plan(ctx,entry->syntax,value->type,expected,&conversion) &&
        source_conversion_emit(ctx,&conversion,value) && source_query_expression(ctx,entry->syntax,value->type);
    if(ok && source_root_type(ctx,value->type,0))
        ok=source_root_query_record(ctx,value->id,ctx->query.expression_count-1,true,false);
    --ctx->depth;ctx->active_expression=parent;
    plan->state=ok?SOURCE_TERM_GROUND:SOURCE_TERM_FAILED;
    if(ok) {plan->value=*value;plan->exit=ctx->bodies[ctx->function].current_block;plan->type_ready=true;plan->ground_type=value->type;}
    return ok;
}
