/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_derive_execute.inc.c - Real checked dependency thunk evaluation
 */
static bool source_comptime_failure(SourceContext *,AstNode *,XrXirCtfeStatus);
static void source_derive_emitter_free(SourceContext *ctx) {
    source_vector_retired_free(ctx);
    while (ctx->memory) {
        SourceMemory *next=ctx->memory->next;xr_compile_resources_free(ctx->memory);ctx->memory=next;
    }
}
static bool source_derive_emitter(SourceContext *facts,SourceDeriveJob *job,SourceContext *ctx) {
    const XrXirModule *library=job->library->library;
    if (facts->compile.limits.functions<2 || library->function_count>facts->compile.limits.functions-2)
        return source_fail(facts,job->owner->node,XR_XIR_BUDGET,"derive startup function budget exhausted");
    ctx->compile=facts->compile;ctx->linkage_kind=XR_XIR_LIBRARY;
    ctx->remaining_blocks=facts->remaining_blocks;ctx->remaining_instructions=facts->remaining_instructions;
    ctx->function_count=ctx->function_capacity=library->function_count+2;
    ctx->functions=source_alloc(ctx,ctx->function_count,sizeof(*ctx->functions));
    ctx->identities=source_alloc(ctx,ctx->function_count,sizeof(*ctx->identities));
    ctx->generics=source_alloc(ctx,ctx->function_count,sizeof(*ctx->generics));
    ctx->bodies=source_alloc(ctx,ctx->function_count,sizeof(*ctx->bodies));
    if (!ctx->functions || !ctx->identities || !ctx->generics || !ctx->bodies ||
        !source_copy_bytes(ctx,NULL,ctx->functions,library->functions,library->function_count*sizeof(*ctx->functions)) ||
        !source_copy_bytes(ctx,NULL,ctx->identities,library->declarations->functions,library->function_count*sizeof(*ctx->identities))) return false;
    ctx->has_generics=library->generics!=NULL;
    if (library->generics && !source_copy_bytes(ctx,NULL,ctx->generics,library->generics,
        library->function_count*sizeof(*ctx->generics))) return false;
    if (library->types) ctx->types=*library->types;
    ctx->type_capacity=ctx->types.count;
    XrXirTypeNode *nodes=source_alloc(ctx,ctx->type_capacity,sizeof(*nodes));
    if (!nodes || !source_copy_bytes(ctx,NULL,nodes,ctx->types.nodes,ctx->types.count*sizeof(*nodes))) return false;
    ctx->types.nodes=nodes;
    ctx->literal_count=ctx->literal_capacity=library->declarations->literal_count;
    ctx->literals=source_alloc(ctx,ctx->literal_count,sizeof(*ctx->literals));
    if (!ctx->literals || !source_copy_bytes(ctx,NULL,ctx->literals,library->declarations->literals,
        ctx->literal_count*sizeof(*ctx->literals))) return false;
    ctx->module=library->declarations->functions[job->generator].module;
    ctx->function=library->function_count;
    ctx->functions[ctx->function]=(XrXirFunction){.name="$derive",.name_length=7};
    ctx->identities[ctx->function]=(XrXirFunctionIdentity){.module=ctx->module};
    ctx->bodies[ctx->function].node=job->owner->node;ctx->bodies[ctx->function].module=ctx->module;
    return true;
}
static bool source_derive_thunk(SourceContext *ctx,SourceDeriveJob *job,SourceInfoGraph *graph) {
    const XrXirModule *library=job->library->library;
    const XrXirFunction *generator=&library->functions[job->generator];
    if (generator->parameter_count!=2 || (library->generics &&
        (library->generics[job->generator].parameter_count || library->generics[job->generator].argument_count)))
        return source_fail(ctx,job->owner->node,XR_XIR_BAD_TYPE,"derive generator requires two closed ordinary data parameters");
    SourceValue arguments[2],result;
    if (!source_info_prepare(graph,job->owner->type) || !source_derive_context(graph,job,&arguments[1]) ||
        !source_info_materialize(graph,job->owner->type,&arguments[0])) return false;
    if (generator->parameters[0]!=arguments[0].type || generator->parameters[1]!=arguments[1].type)
        return source_fail(ctx,job->owner->node,XR_XIR_BAD_TYPE,"derive generator parameters do not match TypeInfo and DeriveContext");
    ctx->functions[ctx->function].result=generator->result;
    if (!source_recipe_group(ctx,(XrXirInstruction){.op=XR_XIR_CALL,.type=generator->result,.immediate=job->generator},arguments,2,&result) ||
        !source_recipe_record(ctx,(XrXirInstruction){.op=XR_XIR_RETURN,.args={result.id,0}},NULL)) return false;
    ctx->returned=true;
    if (!finish_body(ctx)) return false;
    uint32_t thunk=ctx->function,entry=thunk+1;
    const XrXirTypeNode *schema=xr_xir_tuple_signature(&ctx->types,generator->result);
    if (!schema || schema->parameter_count!=8 || schema->parameters[0].type!=XR_XIR_I64)
        return source_fail(ctx,job->owner->node,XR_XIR_BAD_TYPE,"derive output requires a versioned ordinary data tuple");
    ctx->functions[entry]=(XrXirFunction){.name="$deriveEntry",.name_length=12,.result=XR_XIR_I64};
    ctx->identities[entry]=(XrXirFunctionIdentity){.module=ctx->module};
    ctx->bodies[entry].node=job->owner->node;ctx->bodies[entry].module=ctx->module;
    ctx->function=entry;ctx->returned=false;SourceValue data,version;
    if (!source_recipe_group(ctx,(XrXirInstruction){.op=XR_XIR_CALL,.type=generator->result,.immediate=thunk},NULL,0,&data) ||
        !source_recipe_record(ctx,(XrXirInstruction){.op=XR_XIR_TUPLE_FIELD,.type=XR_XIR_I64,.args={data.id,0}},&version) ||
        !source_recipe_record(ctx,(XrXirInstruction){.op=XR_XIR_RETURN,.args={version.id,0}},NULL)) return false;
    ctx->returned=true;return finish_body(ctx);
}
static bool source_derive_evaluate(SourceContext *facts,SourceContext *ctx,SourceDeriveJob *job) {
    const XrXirModule *library=job->library->library;XrXirDeclarations declarations=*library->declarations;
    declarations.functions=ctx->identities;declarations.literals=ctx->literals;declarations.literal_count=ctx->literal_count;
    declarations.root_module=ctx->module;declarations.entry_function=ctx->function;
    XrXirModule built=*library;built.stage=XR_XIR_BUILT;built.provenance=NULL;
    built.linkage_kind=XR_XIR_PROGRAM;
    built.functions=ctx->functions;built.function_count=ctx->function_count;built.types=&ctx->types;
    built.generics=ctx->has_generics?ctx->generics:NULL;built.declarations=&declarations;
    XrXirArtifact *checked=NULL,*closed=NULL,*lowered=NULL;XrXirCheckedPacket packet={0};uint8_t identity[32]={0};
    XrXirDiagnostic diagnostic={0};
    const char *stage="checking";
    XrXirStatus status=xr_xir_compile_check_v2(&ctx->compile,&built,job->library->construction,&checked,&diagnostic);
    if (status==XR_XIR_OK) {stage="specialization";status=xr_xir_compile_specialize(checked,&closed,&diagnostic);}
    if (status==XR_XIR_OK) {stage="serialization";status=xr_xir_compile_checked_write(closed,&packet,&diagnostic);}
    if (status==XR_XIR_OK) {
        if (!source_work_units(ctx,NULL,packet.length)) status=XR_XIR_BUDGET;
        else xr_sha256(packet.bytes,packet.length,identity);
    }
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    if (status==XR_XIR_OK) {stage="lowering";status=xr_xir_compile_lower(closed,&target,&lowered,&diagnostic);}
    uint32_t function=UINT32_MAX;
    if (status==XR_XIR_OK) {
        const XrXirModule *module=xr_xir_compile_artifact_module(lowered);
        for (uint32_t f=0;f<module->function_count;++f) {
            if (!source_work(ctx,NULL)) {status=XR_XIR_BUDGET;break;}
            const XrXirFunction *body=&module->functions[f];
            if (module->declarations->functions[f].module!=ctx->module || body->name_length!=7 ||
                !source_span_same(ctx,NULL,body->name,"$derive",7)) continue;
            if (function!=UINT32_MAX || body->parameter_count) {status=XR_XIR_BAD_STRUCTURE;break;}
            function=f;
        }
        if (function==UINT32_MAX) status=XR_XIR_BAD_STRUCTURE;
    }
    if (status==XR_XIR_OK) {
        XrXirCtfeCallRequest request={.evaluation={.artifact=lowered,.function=function,.target=target,
            .limits={XR_XIR_CTFE_MAX_STEPS,XR_XIR_CTFE_MAX_FRAME_BYTES,facts->comptime_output_bytes}},
            .call_depth=XR_XIR_CTFE_MAX_CALL_DEPTH};
        memcpy(request.evaluation.checked_identity,identity,sizeof(identity));job->owns_value=true;
        XrXirCtfeStatus evaluated=xr_xir_compile_ctfe_calls(&request,&job->value);
        if (evaluated!=XR_XIR_CTFE_OK) source_comptime_failure(facts,job->owner->node,evaluated);
    } else {
        char message[192]={0};
        if (status!=XR_XIR_BUDGET && status!=XR_XIR_OUT_OF_MEMORY &&
            source_format(facts,message,sizeof(message),"derive dependency %s failed at function %u block %u instruction %u",
                stage,diagnostic.function,diagnostic.block,diagnostic.instruction)!=XR_DIAG_OK) status=XR_XIR_BUDGET;
        source_fail(facts,job->owner->node,status,message);
    }
    xr_xir_compile_checked_packet_free(&packet);xr_xir_compile_artifact_free(lowered);
    xr_xir_compile_artifact_free(closed);xr_xir_compile_artifact_free(checked);
    return facts->diagnostic.status==XR_XIR_OK;
}
