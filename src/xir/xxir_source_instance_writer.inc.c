/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_instance_writer.inc.c - Definition-born ordinary literal providers
 */
typedef struct SourceInstanceTypes {
    SourceContext *ctx;
    const XrXirTypes *source;
    XrXirType *mapped;
} SourceInstanceTypes;
static bool source_instance_argument(SourceInstanceTypes *input,XrXirType argument,
    uint32_t depth,XrXirType *output) {
    SourceContext *ctx=input->ctx;
    if (!source_work_units(ctx,NULL,sizeof(XrXirTypeNode)+4)) return false;
    if (depth==128) return source_fail(ctx,NULL,XR_XIR_BUDGET,"constructed type substitution depth exhausted");
    if ((uint32_t)argument<XR_XIR_CONSTRUCTED_TYPE_BASE) { *output=argument;return true; }
    const XrXirTypeNode *found=xr_xir_type_node(input->source,argument);
    if (!found || found->parameter_span) return source_fail(ctx,NULL,XR_XIR_BAD_TYPE,"instance argument is not a closed type");
    uint32_t index=(uint32_t)argument-XR_XIR_CONSTRUCTED_TYPE_BASE;
    if (input->mapped[index]==(XrXirType)UINT32_MAX)
        return source_fail(ctx,NULL,XR_XIR_BAD_TYPE,"instance argument has a cyclic inline type dependency");
    if (input->mapped[index]) { *output=input->mapped[index];return true; }
    input->mapped[index]=(XrXirType)UINT32_MAX;XrXirTypeNode node=*found;
    if (node.kind==XR_XIR_TYPE_NOMINAL) {
        if (!source_comptime_nominal_identities(ctx,NULL,input->source))
            return source_fail(ctx,NULL,ctx->diagnostic.status==XR_XIR_OK?XR_XIR_BAD_STRUCTURE:ctx->diagnostic.status,
                "instance argument changed its actual nominal declaration");
        XrXirType *arguments=node.nominal.argument_count?source_alloc(ctx,node.nominal.argument_count,sizeof(*arguments)):NULL;
        if (node.nominal.argument_count && !arguments) return false;
        for (uint32_t a=0;a<node.nominal.argument_count;++a)
            if (!source_instance_argument(input,node.nominal.arguments[a],depth+1,&arguments[a])) return false;
        node.nominal.arguments=arguments;node.nominal.fields=NULL;node.nominal.field_count=0;
    } else if (node.kind==XR_XIR_TYPE_CALLABLE || node.kind==XR_XIR_TYPE_TUPLE) {
        XrXirCallableParameter *parameters=node.parameter_count?source_alloc(ctx,node.parameter_count,sizeof(*parameters)):NULL;
        if (node.parameter_count && !parameters) return false;
        for (uint32_t p=0;p<node.parameter_count;++p) {
            if (!source_work_units(ctx,NULL,sizeof(*parameters))) return false;
            parameters[p].mode=node.parameters[p].mode;
            if (!source_instance_argument(input,node.parameters[p].type,depth+1,&parameters[p].type)) return false;
        }
        node.parameters=parameters;
        if (node.kind==XR_XIR_TYPE_CALLABLE && !source_instance_argument(input,node.result,depth+1,&node.result)) return false;
    } else if (!source_instance_argument(input,node.element,depth+1,&node.element)) return false;
    if (!source_intern_type(ctx,node,output)) return false;
    input->mapped[index]=*output;
    if (node.kind==XR_XIR_TYPE_NOMINAL) {
        /* Only nominal field edges may refer back to an established identity.
         * Complete current declaration matching above precedes receiving the
         * actual closed fields; common checking rederives every substitution. */
        uint32_t count=found->nominal.field_count;
        if (count!=ctx->nominals.declarations[node.nominal.declaration].field_count)
            return source_fail(ctx,NULL,XR_XIR_BAD_TYPE,"instance nominal field closure is incomplete");
        XrXirType *fields=count?source_alloc(ctx,count,sizeof(*fields)):NULL;
        if (count && !fields) return false;
        for (uint32_t f=0;f<count;++f) {
            if (!source_work_units(ctx,NULL,sizeof(*fields)) ||
                !source_instance_argument(input,found->nominal.fields[f],depth+1,&fields[f])) return false;
        }
        /* Receiving field types can move the mutable pool. */
        XrXirTypeNode *owned=(XrXirTypeNode *)&ctx->types.nodes[(uint32_t)*output-XR_XIR_CONSTRUCTED_TYPE_BASE];
        owned->nominal.fields=fields;owned->nominal.field_count=count;
    }
    return true;
}
static bool source_instance_literal(SourceContext *ctx,uint32_t index) {
    const XirCtfeInstanceView *view=&ctx->instance_constants->views[index];
    uint32_t definition=view->definition_function;
    if (definition>=ctx->function_count || !ctx->bodies[definition].node ||
        ctx->functions[definition].parameter_count)
        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"instance has no current zero-argument Source definition");
    const XrXirOrigin *origin=&view->checked->module.provenance->origins[view->instance_function];
    if (origin->argument_count!=ctx->generics[definition].parameter_count)
        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"instance Source binder changed");
    AstNode *node=ctx->bodies[definition].node;
    XrXirType *arguments=source_alloc(ctx,origin->argument_count,sizeof(*arguments));
    if (origin->argument_count && !arguments) return false;
    const XrXirTypes *from=view->checked->module.types;
    SourceInstanceTypes importer={ctx,from,from && from->count?source_alloc(ctx,from->count,sizeof(XrXirType)):NULL};
    if (from && from->count && !importer.mapped) return false;
    for (uint32_t a=0;a<origin->argument_count;++a)
        if (!source_instance_argument(&importer,origin->arguments[a],0,&arguments[a])) return false;
    SourceSubstitution substitution={arguments,origin->argument_count};XrXirType result=XR_XIR_UNIT;
    if (!source_substitute(ctx,&substitution,ctx->functions[definition].result,0,&result) ||
        !source_comptime_type_matches(ctx,node,view->lowered->module.types,(XrXirType)view->value->type,result) ||
        !source_comptime_result_type(ctx,node,result))
        return source_fail(ctx,node,ctx->diagnostic.status==XR_XIR_OK?XR_XIR_BAD_TYPE:ctx->diagnostic.status,
            "instance literal result changed its closed Source type");
    uint32_t f=ctx->function_count;
    if (f>=ctx->compile.limits.functions)
        return source_fail(ctx,node,XR_XIR_BUDGET,"instance literal function inventory exhausted");
    if (!source_materialization_storage(ctx,f+1)) return false;
    if (!source_work_units(ctx,node,sizeof(SourceFunction)+sizeof(XrXirFunction)+sizeof(XrXirFunctionIdentity)+4*sizeof(uint32_t))) return false;
    char *name=source_alloc(ctx,40,1);
    if (!name || source_format(ctx,name,40,"$instanceconstant%u",f)!=XR_DIAG_OK)
        return source_fail(ctx,node,ctx->diagnostic.status==XR_XIR_OK?XR_XIR_BUDGET:ctx->diagnostic.status,"instance literal name failed");
    uint32_t module=ctx->bodies[definition].module,nominal=ctx->identities[definition].nominal_owner;
    uint32_t parent=ctx->bodies[definition].declaration;
    ctx->function=f;ctx->module=module;++ctx->function_count;
    ctx->functions[f]=(XrXirFunction){name,(uint32_t)source_text_size(ctx,name),NULL,0,result,NULL,0,NULL,0,NULL,0, XR_XIR_NEVER};
    ctx->identities[f]=(XrXirFunctionIdentity){.module=module,.nominal_owner=nominal,
        .method_kind=nominal?XR_XIR_MEMBER_HELPER:XR_XIR_NON_MEMBER};
    ctx->bodies[f].node=node;ctx->bodies[f].module=module;
    ctx->bodies[f].type_owner=ctx->bodies[definition].type_owner;
    SourceName declaration={.name=name,.node=node};
    if (!source_query_declare(ctx,&declaration,XR_XIR_SOURCE_FUNCTION,parent,source_query_range(ctx,node,NULL))) return false;
    ctx->bodies[f].declaration=declaration.declaration;
    if (!source_query_syntax_role(ctx,declaration.declaration,XR_XIR_SOURCE_SYNTAX_CLOSURE,0)) return false;
    ((XrXirSourceDeclaration *)ctx->query.declarations)[declaration.declaration-1].type=source_query_type(ctx,result);
    ctx->locals=ctx->scope=NULL;ctx->facts=NULL;ctx->epochs=NULL;ctx->depth=0;
    ctx->active_expression=NULL;ctx->effect_argument=NULL;ctx->loop=NULL;
    ctx->type_scope=(SourceTypeScope){0};ctx->returned=false;ctx->flow_alternatives=0;
    SourceValue value={0};
    if (!source_comptime_constant(ctx,node,view->value,result,&value) ||
        !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_RETURN,XR_XIR_UNIT,{result==XR_XIR_UNIT?0:value.id},{0},0,{0}},NULL)) return false;
    ctx->returned=true;
    if (!finish_body(ctx)) return false;
    ctx->instance_constants->providers[index]=f;return true;
}
static bool source_instance_literals_finish(SourceContext *ctx) {
    if (!ctx->instance_constants) return true;
    if (ctx->comptime_count || ctx->materializations)
        return source_fail(ctx,NULL,XR_XIR_BAD_TYPE,"instance literals require a complete ordinary Source definition graph");
    if (ctx->function_count>ctx->compile.limits.functions ||
        ctx->instance_constants->count>ctx->compile.limits.functions-ctx->function_count)
        return source_fail(ctx,NULL,XR_XIR_BUDGET,"instance literal function inventory exhausted");
    if (!source_work_units(ctx,NULL,sizeof(SourceComptimeScope)+sizeof(ctx->materialization_begin))) return false;
    ctx->materialization_begin=ctx->function_count+ctx->instance_constants->count;
    SourceComptimeScope saved=source_comptime_scope(ctx);bool ok=true;
    for (uint32_t i=0;i<ctx->instance_constants->count;++i)
        if (!source_instance_literal(ctx,i)) { ok=false;break; }
    source_comptime_restore(ctx,&saved);return ok;
}
