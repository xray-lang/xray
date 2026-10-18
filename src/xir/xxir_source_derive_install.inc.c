/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_derive_install.inc.c - Unchecked generated body installation
 */
static bool source_derive_function_capacity(SourceContext *ctx,uint32_t additional) {
    if (ctx->function_count>ctx->compile.limits.functions || additional>ctx->compile.limits.functions-ctx->function_count)
        return source_fail(ctx,NULL,XR_XIR_BUDGET,"derived function inventory exhausted");
    uint32_t capacity=ctx->function_capacity;
    if (additional>ctx->compile.limits.functions-capacity) capacity=ctx->compile.limits.functions;
    else capacity+=additional;
    if (capacity==ctx->function_capacity) return true;
    void *old[]={ctx->functions,ctx->bodies,ctx->identities,ctx->generics};
    size_t sizes[]={sizeof(*ctx->functions),sizeof(*ctx->bodies),sizeof(*ctx->identities),sizeof(*ctx->generics)};
    void *arrays[4];
    for (uint32_t i=0;i<4;++i) {
        arrays[i]=source_alloc(ctx,capacity,sizes[i]);
        if (!arrays[i] || !source_copy_bytes(ctx,NULL,arrays[i],old[i],ctx->function_count*sizes[i])) return false;
    }
    ctx->functions=arrays[0];ctx->bodies=arrays[1];ctx->identities=arrays[2];ctx->generics=arrays[3];
    ctx->function_capacity=capacity;
    for (uint32_t i=0;i<4;++i) source_vector_retire(ctx,old[i]);
    return true;
}
static bool source_derive_method(SourceContext *ctx,SourceDeriveJob *job,
    const XrXirGeneratedDataView *data,uint32_t ordinal,uint32_t function) {
    uint32_t member=data->members[ordinal];SourceName *owner=job->owner;
    if (member==UINT32_MAX) {
        ctx->identities[function].method_kind=XR_XIR_NON_MEMBER;return true;
    }
    if (member>=job->requirement_count || data->roots[ordinal]!=ordinal)
        return source_fail(ctx,owner->node,XR_XIR_BAD_TYPE,"derived method must name one authentic requirement root");
    const XrXirInterfaceRequirement *requirement=xr_xir_interface_closure_requirement(job->closure,member);
    const XrXirFunction *body=&ctx->functions[function];
    if (body->name_length!=requirement->name.length ||
        !source_span_same(ctx,owner->node,body->name,requirement->name.bytes,body->name_length))
        return source_fail(ctx,owner->node,XR_XIR_BAD_TYPE,"derived method name disagrees with its selected requirement");
    SourceName *method=add_name(ctx,&ctx->nominal_methods[owner->index],body->name,owner->node);
    if (!method) return false;
    method->kind=SOURCE_FUNCTION;method->module=owner->module;method->index=function;
    ctx->identities[function].nominal_owner=owner->index+1;
    ctx->identities[function].method_kind=XR_XIR_READ_METHOD;
    ctx->identities[function].exported=ctx->nominals.declarations[owner->index].exported;
    ctx->function=function;
    XrXirSourceRange trigger={owner->module,job->attribute->line,job->attribute->column,
        job->attribute->line,job->attribute->column+6};
    if (!source_query_declare(ctx,method,XR_XIR_SOURCE_FUNCTION,owner->declaration,trigger) ||
        !source_query_syntax_role(ctx,method->declaration,XR_XIR_SOURCE_SYNTAX_DERIVED_METHOD,0)) return false;
    ctx->bodies[function].declaration=method->declaration;
    for (uint32_t p=0;p<body->parameter_count;++p) {
        char *name=source_alloc(ctx,32,1);if (!name) return false;
        if (source_format(ctx,name,32,p?"argument%u":"this",p)!=XR_DIAG_OK)
            return source_fail(ctx,owner->node,XR_XIR_BUDGET,"derive parameter display budget exhausted");
        SourceName parameter={.name=name,.node=owner->node};
        XrXirType type=body->parameters[p];
        parameter.mutable=xr_xir_type_is_cell(&ctx->types,type);
        if (parameter.mutable) type=xr_xir_cell_element(&ctx->types,type);
        if (!source_query_declare(ctx,&parameter,XR_XIR_SOURCE_PARAMETER,method->declaration,
            (XrXirSourceRange){owner->module,0,0,0,0})) return false;
        ((XrXirSourceDeclaration *)ctx->query.declarations)[parameter.declaration-1].type=source_query_type(ctx,type);
        if (!p && !source_query_syntax_role(ctx,parameter.declaration,XR_XIR_SOURCE_SYNTAX_RECEIVER,0)) return false;
    }
    return source_query_parameters(ctx,method->declaration);
}
static bool source_derive_install(SourceContext *ctx,SourceDeriveJob *job) {
    const XrXirGeneratedDataView *data=xr_xir_compile_generated_data_view(job->data);
    uint32_t first=ctx->function_count,bindings=ctx->derivations.count;
    if (!source_derive_function_capacity(ctx,data->function_count) || data->function_count>UINT32_MAX-bindings)
        return source_fail(ctx,job->owner->node,XR_XIR_BUDGET,"derived binding inventory exhausted");
    XrXirDerivationGrant *records=source_alloc(ctx,bindings+data->function_count,sizeof(*records));
    if (!records || !source_copy_bytes(ctx,NULL,records,ctx->derivations.records,bindings*sizeof(*records))) return false;
    if (data->literal_count) {
        if (data->literal_count>UINT32_MAX-ctx->literal_count)
            return source_fail(ctx,job->owner->node,XR_XIR_BUDGET,"derived literal inventory exhausted");
        uint32_t count=ctx->literal_count+data->literal_count;
        XrXirLiteral *literals=source_alloc(ctx,count,sizeof(*literals));
        if (!literals || !source_copy_bytes(ctx,NULL,literals,ctx->literals,ctx->literal_count*sizeof(*literals)) ||
            !source_copy_bytes(ctx,NULL,literals+ctx->literal_count,data->literals,data->literal_count*sizeof(*literals))) return false;
        source_vector_retire(ctx,ctx->literals);ctx->literals=literals;ctx->literal_count=ctx->literal_capacity=count;
    }
    uint32_t instruction=0;
    for (uint32_t i=0;i<data->function_count;++i) {
        uint32_t f=first+i;const XrXirFunction *body=&data->functions[i];
        if (!source_work(ctx,job->owner->node)) return false;
        if (body->block_count>ctx->remaining_blocks || body->instruction_count>ctx->remaining_instructions)
            return source_fail(ctx,job->owner->node,XR_XIR_BUDGET,"derived body budget exhausted");
        ctx->remaining_blocks-=body->block_count;ctx->remaining_instructions-=body->instruction_count;
        ctx->functions[f]=*body;ctx->generics[f]=data->generics[i];
        ctx->bodies[f]=(SourceFunction){.node=job->owner->node,.module=job->owner->module,
            .parameters=(XrXirType *)body->parameters,.region_sealed=true};
        ctx->identities[f]=(XrXirFunctionIdentity){.module=job->owner->module,.derivation=bindings+i+1};
        XrXirGeneratedOrigin *origins=source_alloc(ctx,body->instruction_count,sizeof(*origins));
        if (!origins) return false;
        for (uint32_t n=0;n<body->instruction_count;++n)
            origins[n]=source_derive_origin(ctx,job,data->instruction_fields[instruction++]);
        records[bindings+i]=(XrXirDerivationGrant){f,job->policy,bindings+i,bindings+data->roots[i],
            job->owner->type,{source_derive_origin(ctx,job,-1),origins,body->instruction_count}};
    }
    ctx->function_count+=data->function_count;
    ctx->derivations.records=records;ctx->derivations.count=bindings+data->function_count;
    for (uint32_t i=0;i<data->function_count;++i)
        if (!source_derive_method(ctx,job,data,i,first+i)) return false;
    return ctx->diagnostic.status==XR_XIR_OK;
}
static bool source_derivations_execute(SourceContext *ctx) {
    const SourceDeriveJob *input=ctx->derive_input;
    for (SourceDeriveJob *job=ctx->derive_jobs;job;job=job->next) {
        ctx->module=job->owner->module;SourceContext emitter={0};
        SourceInfoGraph graph={.ctx=&emitter,.facts=ctx,.syntax=job->owner->node,
            .policy=&ctx->derivations.policies[job->policy]};
        bool ok=source_derive_emitter(ctx,job,&emitter) && source_derive_thunk(&emitter,job,&graph);
        if (ok && ctx->derive_input) {
            if (!input || input->owner->node!=job->owner->node || input->attribute!=job->attribute ||
                input->policy!=job->policy || input->generator!=job->generator ||
                input->library->artifact!=job->library->artifact)
                ok=source_fail(ctx,job->owner->node,XR_XIR_BAD_STRUCTURE,"derive receiving occurrence changed identity");
            else {job->value=input->value;input=input->next;}
        } else if (ok) ok=source_derive_evaluate(ctx,&emitter,job);
        ctx->remaining_blocks=emitter.remaining_blocks;ctx->remaining_instructions=emitter.remaining_instructions;
        if (!ok && ctx->diagnostic.status==XR_XIR_OK)
            source_fail(ctx,job->owner->node,emitter.diagnostic.status,"derive startup failed");
        source_derive_emitter_free(&emitter);
        if (!ok) return false;
        XrXirGeneratedDataRequest request={.value=&job->value,.types=&ctx->types,
            .references=graph.types,.reference_count=graph.count,.callees=job->callees,.callee_count=job->callee_count,
            .first_function=ctx->function_count,.first_literal=ctx->literal_count,
            .field_count=ctx->nominals.declarations[job->owner->index].field_count,.method_count=job->requirement_count};
        XrXirStatus status=xr_xir_compile_generated_data_decode(&ctx->compile,&request,&job->data);
        if (status!=XR_XIR_OK) return source_fail(ctx,job->owner->node,status,"ordinary derive output has invalid logical body data");
        if (!source_derive_install(ctx,job)) return false;
    }
    return !input || source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"derive receiving occurrence coverage changed");
}
