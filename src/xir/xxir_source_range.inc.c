/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_range.inc.c - Canonical range expressions through ordinary class calls
 *
 * KEY CONCEPT:
 *   Literal authority selects the owned prelude constructor; lexical names and
 *   ordinary constructor access checks keep their existing meaning.
 */
static bool source_range_is(SourceContext *ctx,XrXirType type) {
    SourceName *name=source_prelude_name(ctx,"Range");
    const XrXirTypeNode *node=xr_xir_type_node(&ctx->types,type);
    return name && name->kind==SOURCE_NOMINAL && name->module+1==ctx->prelude_module &&
        node && node->kind==XR_XIR_TYPE_NOMINAL && node->nominal.declaration==name->index &&
        !node->nominal.argument_count;
}
static bool source_range_length(SourceContext *ctx,AstNode *node,SourceValue receiver,SourceValue *value) {
    if(!source_range_is(ctx,receiver.type))
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"len requires the actual canonical Range declaration");
    const XrXirTypeNode *nominal=xr_xir_type_node(&ctx->types,receiver.type);
    SourceName *method=find_name(ctx,ctx->nominal_methods[nominal->nominal.declaration],"count");
    if(!method) return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"canonical Range count method is missing");
    const XrXirFunctionIdentity *identity=&ctx->identities[method->index];
    const XrXirFunction *function=&ctx->functions[method->index];
    if(identity->nominal_owner!=nominal->nominal.declaration+1 || identity->member_access ||
        !identity->exported || identity->method_kind!=XR_XIR_READ_METHOD || function->parameter_count!=1 ||
        function->parameters[0]!=receiver.type || function->result!=XR_XIR_I64 || function->error_type!=XR_XIR_NEVER)
        return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"canonical Range count signature is invalid");
    return source_recipe_group(ctx,(XrXirInstruction){XR_XIR_CALL,XR_XIR_I64,{0},{0},method->index,{0}},&receiver,1,value);
}
static bool source_range_type(SourceContext *ctx,AstNode *node,XrXirType *type) {
    SourceName *name=source_prelude_name(ctx,"Range");
    if(!name || name->kind!=SOURCE_NOMINAL || name->module+1!=ctx->prelude_module)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"canonical Range is outside this module's authority");
    XrXirStatus status=library_range_record(&ctx->nominals.declarations[name->index]);
    if(status!=XR_XIR_OK) return source_fail(ctx,node,status,"canonical Range fields are invalid");
    *type=name->type;return true;
}
static bool source_range_value(SourceContext *ctx,AstNode *node,SourceValue *value) {
    SourceExpressionPlan *plan=ctx->active_expression;
    XrXirType type;
    if(!source_range_type(ctx,node,&type)) return false;
    const XrXirTypeNode *nominal=xr_xir_type_node(&ctx->types,type);
    uint32_t constructor=ctx->nominal_constructors[nominal->nominal.declaration];
    if(!constructor || ctx->identities[constructor].nominal_owner!=nominal->nominal.declaration+1 ||
        ctx->identities[constructor].method_kind!=XR_XIR_CONSTRUCTOR ||
        ctx->identities[constructor].member_access!=XR_XIR_MEMBER_PUBLIC ||
        ctx->identities[constructor].exported!=1 ||
        ctx->functions[constructor].parameter_count!=3)
        return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"canonical Range constructor is invalid");
    SourceValue arguments[3];
    if(!plan->left || !plan->right || !source_plan_complete(ctx,plan->left,&arguments[0]) ||
        !source_plan_complete(ctx,plan->right,&arguments[1])) return false;
    if(arguments[0].type!=XR_XIR_I64 || arguments[1].type!=XR_XIR_I64)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"range endpoints must be i64");
    if(!source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CONST_BOOL,XR_XIR_BOOL,{0},{0},
        node->as.range.inclusive_end?1:0,{0}},&arguments[2])) return false;
    SourceCallPlan call={.family=SOURCE_CALL_CONSTRUCTOR,
        .function_parameters=ctx->functions[constructor].parameters,.declaration={.function=constructor}};
    if(!source_hir_call_prepare(ctx,node,&call,3))return false;
    for(uint32_t p=0;p<3;++p) {
        XrHirCallUseInput input={.kind=XR_HIR_CALL_ACCESS,.formal=p,.argument=p,.access=XR_CALL_ARG_PLAIN};
        if(!source_hir_call_use(ctx,node,&call,input,NULL,NULL))return false;
    }
    return source_constructor_emit(ctx,node,&(SourceConstructorEmission){type,constructor,3,arguments,call.signature},value);
}
/* The authenticated canonical constructor/count bodies establish immutable unit
 * steps. Indexing reads the captured receiver after evaluating its index once. */
static bool source_range_index(SourceContext *ctx,AstNode *node,SourceValue receiver,
    AstNode *index,SourceValue *value) {
    SourceValue key={0},length={0},start={0};
    if (!source_range_is(ctx,receiver.type))
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"index requires the actual canonical Range declaration");
    if (!source_plan_expression(ctx,index,(SourceExpectedType){true,XR_XIR_I64,false,false},&key)) return false;
    if (key.type!=XR_XIR_I64) return source_fail(ctx,index,XR_XIR_BAD_TYPE,"Range index requires exact i64");
    uint32_t declaration=xr_xir_type_node(&ctx->types,receiver.type)->nominal.declaration;
    XrXirStatus status=library_range_record(&ctx->nominals.declarations[declaration]);
    if (status!=XR_XIR_OK) return source_fail(ctx,node,status,"canonical Range fields are invalid");
    if (!source_range_length(ctx,node,receiver,&length) ||
        !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_INDEX_CHECK,XR_XIR_UNIT,{key.id,length.id},{0},0,{0}},NULL) ||
        !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CLASS_GET,XR_XIR_I64,{receiver.id,0},{0},0,{0}},&start)) return false;
    return source_recipe_record(ctx,(XrXirInstruction){XR_XIR_ADD_INT,XR_XIR_I64,{start.id,key.id},{0},0,{0}},value);
}
