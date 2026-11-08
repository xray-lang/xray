/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_task_timeout.inc.c - Owned timer race on ordinary Task primitives
 *
 * KEY CONCEPT:
 *   Closed helpers own every argument. The existing first aggregate arbitrates
 *   terminal outcomes; ordinary cleanup cancels the input and private helpers.
 */
static bool source_timeout_function(SourceContext *ctx,AstNode *node,XrXirType parameter,
    XrXirType result,XrXirType error,uint32_t *output) {
    if(ctx->function_count>=ctx->function_capacity||ctx->function_count>=ctx->compile.limits.functions)
        return source_fail(ctx,node,XR_XIR_BUDGET,"Task timeout helper inventory exhausted");
    uint32_t index=ctx->function_count++;
    char spelling[48];
    snprintf(spelling,sizeof(spelling),"$task_timeout_%u",index);
    const char *name=source_owned_text(ctx,spelling);
    XrXirType *parameters=source_alloc(ctx,1,sizeof(*parameters));
    XrXirSourceType *query_parameter=source_alloc(ctx,1,sizeof(*query_parameter));
    if(!name||!parameters||!query_parameter)return false;
    *parameters=parameter;*query_parameter=(XrXirSourceType){parameter,0,true};
    SourceName declaration={.name=name,.node=node,.module=ctx->module};
    if(!source_query_declare(ctx,&declaration,XR_XIR_SOURCE_FUNCTION,
        ctx->bodies[ctx->function].declaration,source_query_range(ctx,node,NULL))||
        !source_query_syntax_role(ctx,declaration.declaration,XR_XIR_SOURCE_SYNTAX_NONE,0))return false;
    ctx->functions[index]=(XrXirFunction){.name=name,.name_length=(uint32_t)strlen(name),
        .parameters=parameters,.parameter_count=1,.result=result,.error_type=error};
    ctx->identities[index]=(XrXirFunctionIdentity){.module=ctx->module};
    ctx->bodies[index]=(SourceFunction){.node=node,.module=ctx->module,.parameters=parameters,
        .declaration=declaration.declaration};
    XrXirSourceDeclaration *query=(XrXirSourceDeclaration *)&ctx->query.declarations[declaration.declaration-1];
    query->type=(XrXirSourceType){result,0,true};query->parameters=query_parameter;query->parameter_count=1;
    *output=index;return true;
}
static bool source_timeout_register(SourceContext *ctx,uint32_t helper,SourceValue task) {
    SourceFunction *body=ctx->bodies+ctx->function;
    if(!body->block_count&&!begin_block(ctx))return false;
    if(!source_recipe_group(ctx,(XrXirInstruction){.op=XR_XIR_CLEANUP_REGISTER,.type=XR_XIR_UNIT,
        .targets={body->block_count},.immediate=helper},&task,1,NULL))return false;
    body->frontier=body->count;return begin_block(ctx);
}
static bool source_timeout_cancel_body(SourceContext *ctx,uint32_t function,uint32_t owner) {
    ctx->function=function;ctx->returned=false;
    ctx->identities[function].cleanup_owner=owner+1;
    ctx->identities[function].capture_owner=owner+1;ctx->identities[function].capture_count=1;
    ctx->identities[function].promises=XR_XIR_FUNCTION_NO_SUSPEND;
    if(!source_recipe_append(ctx,(XrXirInstruction){.op=XR_XIR_TASK_CANCEL,.type=XR_XIR_UNIT,.args={0}},NULL)||
        !source_recipe_append(ctx,(XrXirInstruction){.op=XR_XIR_RETURN,.type=XR_XIR_UNIT},NULL))return false;
    ctx->returned=true;return finish_body(ctx);
}
static bool source_timeout_timer_body(SourceContext *ctx,XrXirType timeout) {
    SourceFunction *body=ctx->bodies+ctx->function;
    SourceValue zero,positive,maximum,large,error;
    if(!source_recipe_append(ctx,(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64},&zero)||
        !source_recipe_append(ctx,(XrXirInstruction){.op=XR_XIR_GT_INT,.type=XR_XIR_BOOL,.args={0,zero.id}},&positive))return false;
    uint32_t first=body->count;
    if(!source_recipe_append(ctx,(XrXirInstruction){.op=XR_XIR_BRANCH,.type=XR_XIR_UNIT,.args={positive.id},.targets={1,0}},NULL)||
        !begin_block(ctx)||
        !source_recipe_append(ctx,(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=86400000},&maximum)||
        !source_recipe_append(ctx,(XrXirInstruction){.op=XR_XIR_GT_INT,.type=XR_XIR_BOOL,.args={0,maximum.id}},&large)||
        !source_recipe_append(ctx,(XrXirInstruction){.op=XR_XIR_BRANCH,.type=XR_XIR_UNIT,.args={large.id},.targets={2,3}},NULL)||
        !begin_block(ctx)||
        !source_recipe_append(ctx,(XrXirInstruction){.op=XR_XIR_TIMER_AFTER_MS,.type=XR_XIR_UNIT,.args={maximum.id}},NULL))return false;
    uint32_t bounded=body->count;
    if(!source_recipe_append(ctx,(XrXirInstruction){.op=XR_XIR_JUMP,.type=XR_XIR_UNIT,.targets={4}},NULL)||
        !begin_block(ctx)||
        !source_recipe_append(ctx,(XrXirInstruction){.op=XR_XIR_TIMER_AFTER_MS,.type=XR_XIR_UNIT,.args={0}},NULL)||
        !source_recipe_append(ctx,(XrXirInstruction){.op=XR_XIR_JUMP,.type=XR_XIR_UNIT,.targets={4}},NULL)||
        !begin_block(ctx))return false;
    body->recipes[first].instruction.targets[1]=4;
    body->recipes[bounded].instruction.targets[0]=4;
    return source_recipe_group(ctx,(XrXirInstruction){.op=XR_XIR_ENUM_NEW,.type=timeout},NULL,0,&error)&&
        source_recipe_append(ctx,(XrXirInstruction){.op=XR_XIR_THROW,.type=XR_XIR_UNIT,.args={error.id}},NULL);
}
static bool source_timeout_bodies(SourceContext *ctx,AstNode *node,const uint32_t *functions,
    XrXirType timeout,XrXirType task,XrXirType value) {
    SourceComptimeScope saved=source_comptime_scope(ctx);SourceTaskScope *scope=ctx->task_scope;
    ctx->locals=ctx->scope=NULL;ctx->facts=NULL;ctx->epochs=NULL;ctx->loop=NULL;
    ctx->active_expression=ctx->effect_argument=NULL;ctx->task_scope=NULL;
    ctx->depth=ctx->flow_alternatives=0;ctx->type_scope=(SourceTypeScope){0};
    bool ok=source_timeout_cancel_body(ctx,functions[2],functions[0])&&
        source_timeout_cancel_body(ctx,functions[3],saved.function)&&
        source_timeout_cancel_body(ctx,functions[4],saved.function);
    SourceValue input={.id=0,.type=task},result={0};
    ctx->function=functions[0];ctx->returned=false;
    if(ok)ok=source_timeout_register(ctx,functions[2],input)&&
        source_task_await_emit(ctx,node,input,&result,true)&&
        source_recipe_append(ctx,(XrXirInstruction){.op=XR_XIR_RETURN,.type=XR_XIR_UNIT,
            .args={value==XR_XIR_UNIT?0:result.id}},NULL);
    ctx->returned=true;if(ok)ok=finish_body(ctx);
    ctx->function=functions[1];ctx->returned=false;
    if(ok)ok=source_timeout_timer_body(ctx,timeout);
    ctx->returned=true;if(ok)ok=finish_body(ctx);
    source_comptime_restore(ctx,&saved);ctx->task_scope=scope;return ok;
}
static bool source_timeout_read_argument(SourceContext *ctx,AstNode *site,AstNode *argument) {
    for(uint32_t depth=0;argument&&argument->type==AST_GROUPING;++depth) {
        if(depth>=128)return source_fail(ctx,site,XR_XIR_BUDGET,"Task timeout argument nesting exhausted");
        if(!(depth%64u)&&!source_request_poll(ctx,site))return false;
        argument=argument->as.grouping;
    }
    if(!argument||argument->type==AST_MOVE_EXPR||
        (argument->type==AST_VARIABLE&&argument->as.variable.access_marker_span.line>0)||
        (argument->type==AST_THIS_EXPR&&argument->as.this_expr.access_marker_span.line>0))
        return source_fail(ctx,site,XR_XIR_BAD_TYPE,"Task.timeout takes ordinary readonly Task and i64 arguments");
    return true;
}
static bool source_task_timeout_call(SourceContext *ctx,AstNode *node,
    const XrNativeMemberDeclaration *member,SourceValue *output) {
    CallExprNode *call=&node->as.call_expr;
    if(!member->is_public||!member->is_static||!member->is_method||!member->lowered||
        member->operation!=XR_NATIVE_OPERATION_TASK_TIMEOUT||member->parameter_count!=2||
        call->arg_count!=2||!call->arguments||call->type_arg_count||call->default_arg_count||
        (call->arg_accesses&&(call->arg_accesses[0]!=XR_CALL_ARG_PLAIN||call->arg_accesses[1]!=XR_CALL_ARG_PLAIN)))
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"Task.timeout requires its Task and i64 duration");
    if(!source_timeout_read_argument(ctx,node,call->arguments[0])||
        !source_timeout_read_argument(ctx,node,call->arguments[1]))return false;
    SourceValue input={0},duration={0};
    if(!expression(ctx,call->arguments[0],&input)||
        !source_plan_expression(ctx,call->arguments[1],(SourceExpectedType){true,XR_XIR_I64,false,false},&duration))return false;
    const XrXirTypeNode *type=xr_xir_type_node(&ctx->types,input.type);
    if(!type||type->kind!=XR_XIR_TYPE_TASK||duration.type!=XR_XIR_I64)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"Task.timeout requires its Task and i64 duration");
    if(type->parameter_span)return source_fail(ctx,node,XR_XIR_UNSUPPORTED,"Task.timeout requires closed Task types");
    XrXirType element=type->element,error=type->result,timeout,combined,aggregate,array;
    SourceName *symbol=source_prelude_name(ctx,"Timeout");
    if(!symbol||symbol->kind!=SOURCE_NOMINAL)return source_fail(ctx,node,XR_XIR_BAD_TYPE,"Task.timeout requires the governed Timeout declaration");
    if(!source_intern_type(ctx,(XrXirTypeNode){.kind=XR_XIR_TYPE_NOMINAL,.nominal={.declaration=symbol->index}},&timeout))return false;
    XrXirCallableParameter errors[2]={{error,0,0},{timeout,0,0}};
    if(!source_intern_type(ctx,(XrXirTypeNode){.kind=XR_XIR_TYPE_UNION,.parameters=errors,.parameter_count=2},&combined)||
        !source_task_type(ctx,node,element,combined,&aggregate)||!source_array_element_type(ctx,aggregate,&array))return false;
    uint32_t functions[5];
    if(!source_timeout_function(ctx,node,input.type,element,combined,functions)||
        !source_timeout_function(ctx,node,XR_XIR_I64,element,combined,functions+1)||
        !source_timeout_function(ctx,node,input.type,XR_XIR_UNIT,XR_XIR_NEVER,functions+2)||
        !source_timeout_function(ctx,node,aggregate,XR_XIR_UNIT,XR_XIR_NEVER,functions+3)||
        !source_timeout_function(ctx,node,input.type,XR_XIR_UNIT,XR_XIR_NEVER,functions+4)||
        !source_timeout_bodies(ctx,node,functions,timeout,input.type,element))return false;
    SourceValue racers[2],inputs,selected;
    if(!source_recipe_group(ctx,(XrXirInstruction){.op=XR_XIR_GO,.type=aggregate,.immediate=functions[0]},&input,1,racers)||
        !source_recipe_group(ctx,(XrXirInstruction){.op=XR_XIR_GO,.type=aggregate,.immediate=functions[1]},&duration,1,racers+1)||
        !source_recipe_group(ctx,(XrXirInstruction){.op=XR_XIR_ARRAY_NEW,.type=array},racers,2,&inputs)||
        !source_task_aggregate_begin(ctx,node,XR_NATIVE_CALLABLE_TASK_FIRST_BEGIN,inputs,&selected))return false;
    uint32_t frontier=ctx->bodies[ctx->function].frontier;
    /* Caller cleanup requests input cancellation before publishing the result;
     * a losing forward task may not have started its own cleanup yet. */
    if(!source_timeout_register(ctx,functions[4],input)||
        !source_timeout_register(ctx,functions[3],selected)||
        !source_task_aggregate_reference(ctx,call->callee,member)||!source_task_await_value(ctx,node,selected,output))return false;
    SourceFunction *body=ctx->bodies+ctx->function;
    if(!source_recipe_append(ctx,(XrXirInstruction){.op=XR_XIR_CLEANUP_LEAVE,.type=XR_XIR_UNIT,
        .targets={body->block_count},.immediate=frontier},NULL))return false;
    body->frontier=frontier;return begin_block(ctx);
}
