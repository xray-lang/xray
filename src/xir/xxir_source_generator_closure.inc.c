/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_generator_closure.inc.c - Ordinary closure factories own latent producer creation
 */
static bool source_generator_closure_captures(SourceContext *ctx,AstNode *node,const SourceCaptureScan *scan) {
    for(const SourceCapture *p=scan->captures;p;p=p->next) {
        if(!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
        uint32_t declaration=p->source->declaration;
        if(p->source->mutable && declaration && declaration<=ctx->query.declaration_count &&
            ctx->query.declarations[declaration-1].kind==XR_XIR_SOURCE_PARAMETER)
            return source_fail(ctx,node,XR_XIR_BAD_TYPE,"generator cannot retain an external ref capture");
    }
    return true;
}
static bool source_generator_closure_factory(SourceContext *ctx,AstNode *node,uint32_t producer,
    bool no_suspend,uint32_t *target) {
    if(ctx->next_closure>=ctx->closure_limit)
        return source_fail(ctx,node,XR_XIR_BUDGET,"generator closure factory capacity exhausted");
    uint32_t factory=ctx->next_closure++,outer=ctx->function;bool returned=ctx->returned;
    SourceFunction *original=&ctx->bodies[producer],*body=&ctx->bodies[factory];
    body->node=node;body->module=original->module;body->type_owner=original->type_owner;
    body->generic_owner=original->generic_owner;body->type_parameters=original->type_parameters;
    body->type_parameter_count=original->type_parameter_count;body->parameters=original->parameters;
    ctx->generics[factory]=ctx->generics[producer];ctx->identities[factory]=ctx->identities[producer];
    ctx->identities[factory].promises=no_suspend ? XR_XIR_FUNCTION_NO_SUSPEND : 0;
    char *name=source_alloc(ctx,40,sizeof(*name));
    if(!name || source_format(ctx,name,40,"$generator_factory%u",factory)!=XR_DIAG_OK)
        return source_fail(ctx,node,XR_XIR_BUDGET,"generator closure factory name exhausted");
    SourceName declaration={0};declaration.name=name;declaration.node=node;
    if(!source_query_declare(ctx,&declaration,XR_XIR_SOURCE_FUNCTION,original->declaration,
        source_query_range(ctx,node,NULL))) return false;
    body->declaration=declaration.declaration;
    const XrXirFunction *before=&ctx->functions[producer];
    ctx->functions[factory]=(XrXirFunction){name,(uint32_t)source_text_size(ctx,name),
        body->parameters,before->parameter_count,before->result,NULL,0,NULL,0,NULL,0, XR_XIR_NEVER};
    uint32_t count=before->parameter_count,generic=ctx->generics[producer].parameter_count;
    SourceValue *arguments=count ? source_alloc(ctx,count,sizeof(*arguments)) : NULL;
    XrXirType *types=generic ? source_alloc(ctx,generic,sizeof(*types)) : NULL;
    if((count && !arguments) || (generic && !types)) return false;
    for(uint32_t p=0;p<count;++p) arguments[p]=(SourceValue){.id=p,.type=body->parameters[p],.place={0}};
    for(uint32_t p=0;p<generic;++p) types[p]=(XrXirType)(XR_XIR_TYPE_PARAMETER_BASE+p);
    ctx->function=factory;ctx->returned=false;
    XrXirInstruction op={XR_XIR_GENERATOR_NEW,before->result,{0},{0},producer,{0}};
    SourceValue value;
    bool ok=source_type_arguments(ctx,node,types,generic,&op) &&
        source_recipe_group(ctx,op,arguments,count,&value) &&
        source_recipe_append(ctx,(XrXirInstruction){XR_XIR_RETURN,XR_XIR_UNIT,{value.id},{0},0,{0}},NULL);
    if(ok) {ctx->returned=true;ok=finish_body(ctx);}
    ctx->function=outer;ctx->returned=returned;
    if(ok) *target=factory;
    return ok;
}
