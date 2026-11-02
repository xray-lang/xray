/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_task_aggregate.inc.c - Governed aggregate creation and ordinary await
 *
 * KEY CONCEPT:
 *   The input Array owns real Tasks. A closed native signature creates an
 *   aggregate handle; the existing await path owns its error continuation.
 */
static bool source_task_aggregate_receiver(SourceContext *ctx,AstNode *callee) {
    AstNode *object=callee->as.member_access.object;
    if(!object||object->type!=AST_VARIABLE||source_native_type_shadowed(ctx,object->as.variable.name))return false;
    const XrNativeTypeDeclaration *native=source_native_find(ctx,object->as.variable.name);
    return native&&native->id==XR_NATIVE_DECLARATION_TASK;
}
static bool source_task_aggregate_type(SourceContext *ctx,AstNode *node,SourceValue input,
    bool all,XrXirType *result) {
    const XrXirTypeNode *array=xr_xir_type_node(&ctx->types,input.type);
    const XrXirTypeNode *task=array&&array->kind==XR_XIR_TYPE_ARRAY?xr_xir_type_node(&ctx->types,array->element):NULL;
    if(!task||task->kind!=XR_XIR_TYPE_TASK)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"Task aggregate requires Array<Task<T,E>>");
    if(xr_xir_type_span(&ctx->types,input.type))
        return source_fail(ctx,node,XR_XIR_UNSUPPORTED,"Task aggregate native specialization requires closed types");
    XrXirType element=task->element,error=task->result;
    if(all&&element==XR_XIR_UNIT)
        return source_fail(ctx,node,XR_XIR_UNSUPPORTED,"Task.all requires the not yet admitted Array<Unit> representation");
    return (!all||source_array_element_type(ctx,element,&element))&&source_task_type(ctx,node,element,error,result);
}
static bool source_task_aggregate_reference(SourceContext *ctx,AstNode *node,
    const XrNativeMemberDeclaration *member) {
    uint32_t declaration=0;
    for(uint32_t i=0;i<ctx->query.declaration_count;++i) {
        if(!(i%64u)&&!source_request_poll(ctx,node))return false;
        const XrXirSourceDeclaration *row=&ctx->query.declarations[i];
        if(row->kind==XR_XIR_SOURCE_MEMBER&&row->parent==ctx->task_declaration&&row->native_identity==member->id) {
            declaration=row->id;break;
        }
    }
    if(!declaration) {
        XrXirType element=(XrXirType)XR_XIR_TYPE_PARAMETER_BASE,error=(XrXirType)(XR_XIR_TYPE_PARAMETER_BASE+1);
        XrXirType task,array,result=element;
        if(!source_intern_type(ctx,(XrXirTypeNode){.kind=XR_XIR_TYPE_TASK,.element=element,.result=error},&task)||
            !source_intern_type(ctx,(XrXirTypeNode){.kind=XR_XIR_TYPE_ARRAY,.element=task},&array)||
            (member->operation==XR_NATIVE_OPERATION_TASK_ALL&&
                !source_intern_type(ctx,(XrXirTypeNode){.kind=XR_XIR_TYPE_ARRAY,.element=element},&result)))return false;
        SourceName symbol={0};symbol.name=source_owned_text(ctx,member->name);
        const char *signature=source_owned_text(ctx,member->signature);
        XrXirSourceType *parameter=source_alloc(ctx,1,sizeof(*parameter));
        if(!symbol.name||!signature||!parameter)return false;
        *parameter=(XrXirSourceType){array,ctx->task_declaration,true};
        if(!source_query_declare(ctx,&symbol,XR_XIR_SOURCE_MEMBER,ctx->task_declaration,
            (XrXirSourceRange){ctx->task_module,(int)member->line,(int)member->column,
                (int)member->line,(int)(member->column+source_text_size(ctx,member->name))}))return false;
        declaration=symbol.declaration;
        XrXirSourceDeclaration *row=(XrXirSourceDeclaration *)&ctx->query.declarations[declaration-1];
        row->native_identity=member->id;row->signature=signature;row->exported=true;
        row->type=(XrXirSourceType){result,ctx->task_declaration,true};row->parameters=parameter;row->parameter_count=1;
        row->generic_parent=ctx->task_declaration;row->generic_parent_count=2;
    }
    return source_query_target_token_reference(ctx,node->as.member_access.object,ctx->task_declaration,XR_XIR_SOURCE_READ)&&
        source_query_target_token_reference(ctx,node,declaration,XR_XIR_SOURCE_CALL);
}
static bool source_task_aggregate_begin(SourceContext *ctx,AstNode *node,uint32_t native,
    SourceValue input,SourceValue *output) {
    bool all=native==XR_NATIVE_CALLABLE_TASK_ALL_BEGIN;
    XrXirType result;
    if(!source_task_aggregate_type(ctx,node,input,all,&result))return false;
    uint32_t target=UINT32_MAX;
    for(uint32_t f=0;f<ctx->function_count;++f) {
        if(!(f%64u)&&!source_request_poll(ctx,node))return false;
        if(ctx->identities[f].native_callable==native&&ctx->identities[f].module==ctx->module&&
            ctx->functions[f].parameter_count==1&&ctx->functions[f].parameters[0]==input.type&&
            ctx->functions[f].result==result) {target=f;break;}
    }
    if(target==UINT32_MAX) {
        if(ctx->function_count>=ctx->function_capacity||ctx->function_count>=ctx->compile.limits.functions)
            return source_fail(ctx,node,XR_XIR_BUDGET,"native aggregate inventory exhausted");
        XrXirType *parameter=source_alloc(ctx,1,sizeof(*parameter));
        char spelling[96];
        snprintf(spelling,sizeof(spelling),"$task_aggregate_%u_%u",native,input.type);
        const char *name=source_owned_text(ctx,spelling);
        if(!parameter||!name)return false;
        *parameter=input.type;target=ctx->function_count++;
        ctx->functions[target]=(XrXirFunction){.name=name,.name_length=(uint32_t)strlen(name),
            .parameters=parameter,.parameter_count=1,.result=result,.error_type=XR_XIR_NEVER};
        ctx->identities[target]=(XrXirFunctionIdentity){.module=ctx->module,
            .promises=XR_XIR_FUNCTION_NO_SUSPEND,.native_callable=native};
        ctx->bodies[target].module=ctx->module;
    }
    return source_recipe_group(ctx,(XrXirInstruction){XR_XIR_CALL,result,{0},{0},target,{0}},&input,1,output);
}
static bool source_task_aggregate_call(SourceContext *ctx,AstNode *node,SourceValue *output) {
    CallExprNode *call=&node->as.call_expr;
    if(!source_native_task_declaration(ctx))return false;
    const XrNativeTypeDeclaration *native=xr_native_declaration_by_id(XR_NATIVE_DECLARATION_TASK);
    const XrNativeMemberDeclaration *member=source_native_member(ctx,native,call->callee->as.member_access.name);
    if(!member||!member->is_public||!member->is_static||!member->is_method||!member->lowered||
        member->receiver!=XR_NATIVE_RECEIVER_READ||member->parameter_count!=1||!member->parameters||
        member->parameters[0].type!=XR_NATIVE_TERM_ARRAY_TASK||member->parameters[0].optional||
        member->parameters[0].variadic||
        (member->operation!=XR_NATIVE_OPERATION_TASK_ALL&&member->operation!=XR_NATIVE_OPERATION_TASK_FIRST)||
        member->result!=(member->operation==XR_NATIVE_OPERATION_TASK_ALL?XR_NATIVE_TERM_ARRAY_ELEMENT:XR_NATIVE_TERM_ELEMENT)||
        call->arg_count!=1||!call->arguments||call->type_arg_count||call->default_arg_count||
        (call->arg_accesses&&call->arg_accesses[0]!=XR_CALL_ARG_PLAIN))
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"Task aggregate requires its governed static recipe");
    uint32_t id=member->operation==XR_NATIVE_OPERATION_TASK_ALL?
        XR_NATIVE_CALLABLE_TASK_ALL_BEGIN:XR_NATIVE_CALLABLE_TASK_FIRST_BEGIN;
    const XrNativeCallableDeclaration *leaf=xr_native_callable_by_id(id);
    if(!leaf||leaf->owner!=native||!source_native_admit(ctx,leaf->owner))
        return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"Task aggregate canonical declaration is missing");
    SourceValue input,task;
    return source_plan_expression(ctx,call->arguments[0],(SourceExpectedType){0},&input)&&
        source_task_aggregate_begin(ctx,node,id,input,&task)&&
        source_task_aggregate_reference(ctx,call->callee,member)&&source_task_await_value(ctx,node,task,output);
}
