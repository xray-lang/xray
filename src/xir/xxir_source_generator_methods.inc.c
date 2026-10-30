/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_generator_methods.inc.c - One visible factory owns each hidden method producer
 */
static bool source_generator_method_defaults(SourceContext *ctx,
    uint32_t producer,uint32_t factory) {
    uint32_t count=ctx->defaults.count,begin=0,end=0;
    XrXirDefaultBinding *records=(XrXirDefaultBinding *)ctx->defaults.records;
    while(begin<count && records[begin].owner<producer) {
        if(!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
        ++begin;
    }
    end=begin;
    while(end<count && records[end].owner==producer) {
        if(!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
        ++end;
    }
    uint32_t moved=end-begin;if(!moved) return true;
    XrXirDefaultBinding *tail=source_alloc(ctx,moved,sizeof(*tail));
    if(!tail || !((ctx)->diagnostic.status == XR_XIR_OK)) return false;
    memcpy(tail,records+begin,(size_t)moved*sizeof(*tail));
    memmove(records+begin,records+end,(size_t)(count-end)*sizeof(*records));
    for(uint32_t n=0;n<moved;++n) {tail[n].owner=factory;records[count-moved+n]=tail[n];}
    return true;
}
static bool source_generator_method_witnesses(SourceContext *ctx,uint32_t producer,uint32_t factory) {
    XrXirImplementation *records=(XrXirImplementation *)ctx->implementations.records;
    uint32_t owner=ctx->identities[producer].nominal_owner;
    for(uint32_t n=0;n<ctx->implementations.count;++n) {
        if(!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
        if(records[n].nominal_declaration+1!=owner) continue;
        XrXirImplementationBinding *bindings=(XrXirImplementationBinding *)records[n].bindings;
        for(uint32_t b=0;b<records[n].binding_count;++b) {
            if(!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
            if(bindings[b].function==producer) bindings[b].function=factory;
        }
    }
    return true;
}
static bool source_generator_method_factory(SourceContext *ctx,SourceName *method) {
    uint32_t producer=method->index,factory=ctx->function_count;
    SourceFunction *original=&ctx->bodies[producer];
    if(factory>=ctx->function_capacity || factory>=ctx->compile.limits.functions)
        return source_fail(ctx,original->node,XR_XIR_BUDGET,"generator method factory capacity exhausted");
    SourceFunction *body=&ctx->bodies[factory];
    body->node=original->node;body->module=original->module;body->type_owner=original->type_owner;
    body->generic_owner=original->generic_owner;body->type_parameters=original->type_parameters;
    body->type_parameter_count=original->type_parameter_count;body->parameters=original->parameters;
    body->declaration=method->declaration;
    ctx->generics[factory]=ctx->generics[producer];
    ctx->identities[factory]=ctx->identities[producer];
    ctx->identities[factory].promises&=~XR_XIR_FUNCTION_GENERATOR;
    ctx->functions[factory]=ctx->functions[producer];
    ctx->functions[factory].error_type=XR_XIR_NEVER;
    ++ctx->function_count;
    char *name=source_alloc(ctx,40,1);
    if(!name || source_format(ctx,name,40,"$generator_method%u",producer)!=XR_DIAG_OK)
        return source_fail(ctx,original->node,XR_XIR_BUDGET,"generator method producer name exhausted");
    /* Both lowered entries retain the one lexical method declaration. The
     * receiver and body locals still belong to that source method. */
    ctx->functions[producer].name=name;ctx->functions[producer].name_length=(uint32_t)source_text_size(ctx,name);
    ctx->identities[producer].exported=0;ctx->identities[producer].method_kind=XR_XIR_MEMBER_HELPER;
    ctx->identities[producer].promises&=~XR_XIR_FUNCTION_NO_SUSPEND;
    if(!source_generator_method_defaults(ctx,producer,factory) ||
        !source_generator_method_witnesses(ctx,producer,factory)) return false;
    method->index=factory;
    uint32_t outer=ctx->function;bool returned=ctx->returned;
    ctx->function=factory;ctx->returned=false;
    if(!source_query_parameters(ctx,method->declaration)) return false;
    uint32_t count=ctx->functions[factory].parameter_count,generic=ctx->generics[factory].parameter_count;
    SourceValue *arguments=count ? source_alloc(ctx,count,sizeof(*arguments)) : NULL;
    XrXirType *types=generic ? source_alloc(ctx,generic,sizeof(*types)) : NULL;
    if((count && !arguments) || (generic && !types)) return false;
    for(uint32_t p=0;p<count;++p) {
        if(!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
        arguments[p]=(SourceValue){.id=p,.type=body->parameters[p],.place={0}};
    }
    for(uint32_t p=0;p<generic;++p) {
        if(!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
        types[p]=(XrXirType)(XR_XIR_TYPE_PARAMETER_BASE+p);
    }
    XrXirInstruction op={XR_XIR_GENERATOR_NEW,ctx->functions[factory].result,{0},{0},producer,{0}};
    SourceValue value;
    bool ok=source_type_arguments(ctx,original->node,types,generic,&op) &&
        source_recipe_group(ctx,op,arguments,count,&value) &&
        source_recipe_append(ctx,(XrXirInstruction){XR_XIR_RETURN,XR_XIR_UNIT,{value.id},{0},0,{0}},NULL);
    if(ok) {ctx->returned=true;ok=finish_body(ctx);}
    ctx->function=outer;ctx->returned=returned;
    return ok;
}
static bool source_generator_methods(SourceContext *ctx) {
    uint32_t outer=ctx->function,module=ctx->module;
    for(uint32_t n=0;n<ctx->nominals.count;++n) {
        if(ctx->nominal_sources[n]->checked_library) continue;
        for(SourceName *method=ctx->nominal_methods[n];method;method=method->next) {
            if(!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
            if(!(ctx->identities[method->index].promises & XR_XIR_FUNCTION_GENERATOR)) continue;
            ctx->module=method->module;
            if(!source_generator_method_factory(ctx,method)) return false;
        }
    }
    ctx->function=outer;ctx->module=module;return true;
}
