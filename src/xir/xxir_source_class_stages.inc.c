/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_class_stages.inc.c - Definition-owned constructor field transactions
 *
 * KEY CONCEPT:
 *   Parent initialization produces fields, never a second class object.
 */
typedef struct SourceClassStageScan {
    SourceContext *ctx;AstNode *parent;uint32_t depth;bool eligible;
} SourceClassStageScan;
static bool source_class_stage_scan(AstNode *node,void *pointer) {
    SourceClassStageScan *scan=pointer;
    if(!node || !scan->eligible)return true;
    if(scan->depth==128)return source_fail(scan->ctx,node,XR_XIR_BUDGET,"constructor field transaction depth exhausted");
    if(node->type==AST_FUNCTION_EXPR || node->type==AST_DEFER_STMT) {scan->eligible=false;return true;}
    if(node->type==AST_THIS_EXPR || (node->type==AST_VARIABLE &&
        source_text_same(scan->ctx,node,node->as.variable.name,"this"))) {
        if(!scan->parent || !((scan->parent->type==AST_MEMBER_ACCESS && scan->parent->as.member_access.object==node) ||
                (scan->parent->type==AST_MEMBER_SET && scan->parent->as.member_set.object==node)))
            scan->eligible=false;
        return true;
    }
    if(node->type==AST_MEMBER_ACCESS && scan->parent && scan->parent->type==AST_CALL_EXPR &&
        scan->parent->as.call_expr.callee==node &&
        (node->as.member_access.object->type==AST_THIS_EXPR ||
        (node->as.member_access.object->type==AST_VARIABLE &&
            source_text_same(scan->ctx,node,node->as.member_access.object->as.variable.name,"this")))) {
        scan->eligible=false;return true;
    }
    AstNode *parent=scan->parent;scan->parent=node;++scan->depth;
    bool ok=xr_ast_for_each_child(node,source_class_stage_scan,scan);
    --scan->depth;scan->parent=parent;return ok;
}
static bool source_class_stage_eligible(SourceContext *ctx,AstNode *node) {
    if(!node || node->type!=AST_CLASS_DECL)return false;
    ClassDeclNode *type=&node->as.class_decl;
    if(type->type_param_count || type->explicit_final)return false;
    for(int f=0;f<type->field_count;++f)
        if(type->fields[f]->type!=AST_FIELD_DECL || type->fields[f]->as.field_decl.is_private ||
            type->fields[f]->as.field_decl.is_protected)return false;
    for(int m=0;m<type->method_count;++m) {
        if(type->methods[m]->type!=AST_METHOD_DECL || !type->methods[m]->as.method_decl.is_constructor)continue;
        MethodDeclNode *method=&type->methods[m]->as.method_decl;
        if(method->is_private || method->is_protected)return false;
        SourceClassStageScan scan={ctx,NULL,0,true};
        return source_class_stage_scan(method->body,&scan) && scan.eligible;
    }
    return false;
}
static bool source_class_stage_declarations(SourceContext *ctx,uint32_t *next) {
    for(uint32_t d=0;d<ctx->nominals.count;++d) {
        SourceName *owner=ctx->nominal_sources[d];
        if(owner->checked_library || !source_class_stage_eligible(ctx,owner->node))continue;
        if(!source_request_poll(ctx,owner->node))return false;
        uint32_t constructor=source_hir_nominal_defaults(ctx,d).constructor;
        if(!constructor || *next>=ctx->first_closure)return source_fail(ctx,owner->node,XR_XIR_BAD_STRUCTURE,"class field helper has no constructor owner");
        uint32_t index=(*next)++;ctx->function=index;ctx->module=owner->module;
        XrXirFunction function=ctx->functions[constructor];
        XrXirType result;uint32_t count=ctx->nominals.declarations[d].field_count;
        XrXirCallableParameter *fields=count?source_alloc(ctx,count,sizeof(*fields)):NULL;if(count&&!fields)return false;
        for(uint32_t f=0;f<count;++f)fields[f]=(XrXirCallableParameter){ctx->nominals.declarations[d].fields[f].type,XR_PARAM_READ};
        if(!source_hir_tuple_type(ctx,fields,count,&result))return false;
        function.name="$class_fields";function.name_length=13;function.result=result;
        ctx->functions[index]=function;
        SourceFunction *body=&ctx->bodies[index];body->node=ctx->bodies[constructor].node;body->module=owner->module;
        body->parameters=ctx->bodies[constructor].parameters;
        if(!source_method_parameter_scope(ctx,body->node,&body->parameter_scope))return false;
        SourceName symbol={0};symbol.name=function.name;symbol.node=body->node;
        if(!source_query_declare(ctx,&symbol,XR_XIR_SOURCE_FUNCTION,owner->declaration,
            source_query_range(ctx,body->node,NULL)))return false;
        body->declaration=symbol.declaration;
        ctx->identities[index]=ctx->identities[constructor];
        ctx->identities[index].exported=0;ctx->identities[index].member_access=XR_XIR_MEMBER_PRIVATE;
        ctx->identities[index].method_kind=XR_XIR_CONSTRUCTOR_FIELDS;
        ctx->bodies[constructor].constructor_stage=index+1;
    }
    return ctx->diagnostic.status==XR_XIR_OK;
}
static bool source_class_stage_result(SourceContext *ctx,AstNode *node,SourceValue *value) {
    uint32_t owner=ctx->identities[ctx->function].nominal_owner-1;
    uint32_t count=ctx->nominals.declarations[owner].field_count;
    if(ctx->nominals.declarations[owner].parent && !ctx->bodies[ctx->function].constructor_super)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"derived construction requires one initial super call");
    SourceValue *fields=count?source_alloc(ctx,count,sizeof(*fields)):NULL;if(count&&!fields)return false;
    for(uint32_t f=0;f<count;++f)if(!source_constructor_read(ctx,f,&fields[f]))return false;
    return source_tuple_construct(ctx,node,ctx->functions[ctx->function].result,fields,count,NULL,value);
}
static bool source_class_stage_call(SourceContext *ctx,AstNode *node,uint32_t helper,
    SourceValue *arguments,SourceValue *value) {
    XrXirFunction function=ctx->functions[helper];
    SourceCallPlan plan={.family=SOURCE_CALL_FUNCTION,.function_parameters=function.parameters,.declaration={.function=helper}};
    if(!source_hir_call_prepare(ctx,node,&plan,function.parameter_count))return false;
    for(uint32_t p=0;p<function.parameter_count;++p) {
        uint32_t mode=xr_xir_declared_parameter_mode(&ctx->types,function.parameters[p],function.consume_parameters,function.consume_parameter_count,p);
        XrHirCallUseInput input={.kind=XR_HIR_CALL_ACCESS,.formal=p,.argument=p,
            .access=mode==XR_PARAM_MOVE?XR_CALL_ARG_MOVE:mode==XR_PARAM_REF?XR_CALL_ARG_REF:XR_CALL_ARG_PLAIN};
        if(!source_hir_call_use(ctx,node,&plan,input,NULL,NULL))return false;
    }
    return source_hir_call_commit(ctx,node,&plan,arguments,function.parameter_count,function.parameter_count) &&
        source_hir_call_group(ctx,plan.signature,(XrXirInstruction){.op=XR_XIR_CALL,.type=function.result,.immediate=helper},arguments,value);
}
static bool source_class_stage_wrapper(SourceContext *ctx) {
    SourceFunction *body=&ctx->bodies[ctx->function];uint32_t helper=body->constructor_stage-1;
    XrXirFunction function=ctx->functions[ctx->function];
    if(!source_constructor_parameters(ctx))return false;
    SourceValue *arguments=function.parameter_count?source_alloc(ctx,function.parameter_count,sizeof(*arguments)):NULL;
    if(function.parameter_count&&!arguments)return false;
    for(uint32_t p=0;p<function.parameter_count;++p)arguments[p]=(SourceValue){.id=p,.type=function.parameters[p]};
    SourceValue tuple;
    if(!source_class_stage_call(ctx,body->node,helper,arguments,&tuple))return false;
    uint32_t owner=ctx->identities[ctx->function].nominal_owner-1,count=ctx->nominals.declarations[owner].field_count;
    SourceValue *fields=count?source_alloc(ctx,count,sizeof(*fields)):NULL;if(count&&!fields)return false;
    for(uint32_t f=0;f<count;++f)if(!source_tuple_projection(ctx,body->node,tuple,f,&fields[f]))return false;
    SourceValue value;
    if(!source_hir_nominal_construct(ctx,body->node,&(SourceNominalEmission){function.result,0,fields,count},&value) ||
        !source_recipe_record(ctx,(XrXirInstruction){.op=XR_XIR_RETURN,.args={value.id}},NULL))return false;
    ctx->returned=true;return true;
}
static bool source_class_field_defaults(SourceContext *ctx,uint32_t first) {
    SourceFunction *body=&ctx->bodies[ctx->function];
    uint32_t owner=ctx->identities[ctx->function].nominal_owner-1,count=ctx->nominals.declarations[owner].field_count;
    XrXirType type=visible_name(ctx,"this")->type;
    for(uint32_t f=first;f<count;++f) {
        uint32_t initializer=source_hir_nominal_defaults(ctx,owner).fields[f];
        if(!initializer)continue;
        SourceValue value;XrXirType storage;
        if(!source_constructor_storage_type(ctx,type,f,&storage) ||
            !source_hir_nominal_helper(ctx,body->node,type,f,&value,NULL) ||
            !source_recipe_record(ctx,(XrXirInstruction){xr_xir_type_is_cell(&ctx->types,storage)?XR_XIR_CELL_LOCAL_WRITE:XR_XIR_LOCAL_WRITE,
                XR_XIR_UNIT,{body->constructor_places[f],value.id},{0},0,{0}},NULL))return false;
    }
    return true;
}
static bool source_class_super(SourceContext *ctx,AstNode *node,SourceValue *value) {
    SourceFunction *body=&ctx->bodies[ctx->function];
    if(ctx->identities[ctx->function].method_kind!=XR_XIR_CONSTRUCTOR_FIELDS ||
        !body->node || body->node->type!=AST_METHOD_DECL || body->constructor_super || node->as.super_call.method_name)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"super requires the initial parent field transaction");
    AstNode *block=body->node->as.method_decl.body;
    AstNode *first=block && block->type==AST_BLOCK && block->as.block.count?block->as.block.statements[0]:NULL;
    if(first && first->type==AST_EXPR_STMT)first=first->as.expr_stmt;
    uint32_t owner=ctx->identities[ctx->function].nominal_owner-1,parent=ctx->nominals.declarations[owner].parent;
    if(!parent || first!=node)return source_fail(ctx,node,XR_XIR_BAD_TYPE,"super must be the first derived constructor statement");
    uint32_t constructor=source_hir_nominal_defaults(ctx,parent-1).constructor,helper=UINT32_MAX;
    if(!constructor || ctx->identities[constructor].member_access!=XR_XIR_MEMBER_PUBLIC)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"parent constructor is not publicly callable");
    for(uint32_t f=0;f<ctx->function_count;++f)
        if(ctx->identities[f].nominal_owner==parent && ctx->identities[f].method_kind==XR_XIR_CONSTRUCTOR_FIELDS) {
            if(helper!=UINT32_MAX)return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"parent field helper is not unique");
            helper=f;
        }
    if(helper==UINT32_MAX)return source_fail(ctx,node,XR_XIR_UNSUPPORTED,"parent constructor has no owned field transaction");
    SourceCallSyntax syntax;
    if(!source_call_syntax(ctx,node,&syntax) || !source_argument_arity(ctx,node,constructor,(uint32_t)syntax.arg_count))return false;
    XrXirFunction function=ctx->functions[helper];SourceCallStorage storage={0};
    if(!source_call_storage(ctx,0,function.parameter_count,&storage))return false;
    SourceCallPlan plan={.family=SOURCE_CALL_FUNCTION,.function_parameters=function.parameters,
        .values=storage.values,.result=function.result,.declaration={.function=helper}};
    if(!source_call_expand(ctx,node,&syntax,&plan,function.parameter_count) ||
        !source_call_plan_arguments(ctx,node,&plan,&syntax,NULL))return false;
    for(uint32_t p=(uint32_t)syntax.arg_count;p<function.parameter_count;++p) {
        SourceSubstitution none={0};
        if(!source_argument_default(ctx,node,constructor,p,&none,&storage.values[p]) ||
            !source_hir_call_use(ctx,node,&plan,(XrHirCallUseInput){.kind=XR_HIR_CALL_ACCESS,
                .formal=p,.argument=p,.access=XR_CALL_ARG_PLAIN},NULL,NULL))return false;
    }
    SourceValue tuple;
    if(!source_hir_call_commit(ctx,node,&plan,storage.values,function.parameter_count,function.parameter_count) ||
        !source_hir_call_group(ctx,plan.signature,(XrXirInstruction){.op=XR_XIR_CALL,.type=function.result,.immediate=helper},storage.values,&tuple))return false;
    uint32_t prefix=ctx->nominals.declarations[parent-1].field_count;
    for(uint32_t f=0;f<prefix;++f) {
        SourceValue field;
        if(!source_tuple_projection(ctx,node,tuple,f,&field) || !source_constructor_store(ctx,f,field))return false;
    }
    body->constructor_super=true;
    if(!source_class_field_defaults(ctx,prefix))return false;
    *value=(SourceValue){.id=UINT32_MAX,.type=XR_XIR_UNIT};return true;
}
