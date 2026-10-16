/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_literal_writer.inc.c - Ordinary typed recipes without syntax nodes
 *
 * KEY CONCEPT:
 *   Current generic substitutions select closed types, never AST interpretation.
 *   Existing private materializer bindings remain intact beside appended bodies.
 */
static bool literal_provider(SourceContext *ctx,const XirCtfeInstanceView *view,XirLiteralBinding *binding) {
    SourceSubstitution substitution={binding->arguments,binding->argument_count};XrXirType result=XR_XIR_UNIT;
    uint32_t original=binding->definition,f=ctx->function_count;
    if (!source_substitute(ctx,&substitution,ctx->functions[original].result,0,&result) ||
        !source_comptime_type_matches(ctx,NULL,view->lowered->module.types,(XrXirType)view->value->type,result))
        return source_fail(ctx,NULL,ctx->diagnostic.status==XR_XIR_OK?XR_XIR_BAD_TYPE:ctx->diagnostic.status,
            "literal result changed its actual closed definition type");
    if (f>=ctx->materialization_begin || !source_materialization_storage(ctx,f+1))
        return source_fail(ctx,NULL,XR_XIR_BUDGET,"literal provider inventory exhausted");
    if (!source_work_units(ctx,NULL,sizeof(XrXirFunction)+sizeof(XrXirFunctionIdentity)+sizeof(*binding))) return false;
    char *name=source_alloc(ctx,40,1);
    if (!name || source_format(ctx,name,40,"$literal%u",f)!=XR_DIAG_OK)
        return source_fail(ctx,NULL,ctx->diagnostic.status==XR_XIR_OK?XR_XIR_BUDGET:ctx->diagnostic.status,"literal provider name failed");
    XrXirFunctionIdentity identity=ctx->identities[original];
    ctx->function=f;ctx->module=identity.module;++ctx->function_count;
    ctx->functions[f]=(XrXirFunction){name,(uint32_t)source_text_size(ctx,name),NULL,0,result,NULL,0,NULL,0,NULL,0};
    ctx->identities[f]=(XrXirFunctionIdentity){.module=identity.module,.nominal_owner=identity.nominal_owner,
        .method_kind=identity.nominal_owner?XR_XIR_MEMBER_HELPER:XR_XIR_NON_MEMBER};
    ctx->bodies[f].module=identity.module;ctx->returned=false;
    if (!source_comptime_result_type(ctx,NULL,result))
        return source_fail(ctx,NULL,ctx->diagnostic.status==XR_XIR_OK?XR_XIR_BAD_TYPE:ctx->diagnostic.status,"literal result is not admitted data");
    SourceValue value={0};
    if (!source_comptime_constant(ctx,NULL,view->value,result,&value) ||
        !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_RETURN,XR_XIR_UNIT,{result==XR_XIR_UNIT?0:value.id},{0},0,{0}},NULL)) return false;
    ctx->returned=true;
    if (!finish_body(ctx)) return false;
    binding->provider=f;return true;
}
static bool literal_construction(SourceContext *ctx,const XrXirConstruction *original) {
    XrXirMaterializationBinding *appended=NULL;uint32_t added=0;
    if (!source_materialization_bindings(ctx,&appended,&added)) return false;
    uint32_t before=original->materialization_count;
    if (added>ctx->compile.limits.instructions || before>ctx->compile.limits.instructions-added)
        return source_fail(ctx,NULL,XR_XIR_BUDGET,"literal binding inventory exhausted");
    XrXirMaterializationBinding *all=before+added?source_alloc(ctx,before+added,sizeof(*all)):NULL;
    if ((before+added && !all) || !source_copy_bytes(ctx,NULL,all,original->materializations,(size_t)before*sizeof(*all)) ||
        !source_copy_bytes(ctx,NULL,all?all+before:NULL,appended,(size_t)added*sizeof(*all))) return false;
    XrXirConstructionSpec input={original->rows,original->count,all,before+added,&original->comptime};
    XrXirStatus status=xr_xir_compile_construction_new_v3(&ctx->compile,&ctx->types,&input,&ctx->construction);
    return status==XR_XIR_OK || source_fail(ctx,NULL,status,"literal construction receiving failed");
}
static XrXirStatus literal_check(SourceContext *ctx,const XrXirArtifact *original,XrXirArtifact **output,XrXirDiagnostic *diagnostic) {
    if (!source_materialization_finish(ctx) || !source_effect_prepare(ctx) ||
        !literal_construction(ctx,original->construction)) return ctx->diagnostic.status;
    XrXirDeclarations declarations=*original->module.declarations;
    declarations.functions=ctx->identities;declarations.literals=ctx->literals;declarations.literal_count=ctx->literal_count;
    XrXirModule built=original->module;built.stage=XR_XIR_BUILT;built.provenance=ctx->effect_evidence;
    built.functions=ctx->functions;built.function_count=ctx->function_count;built.declarations=&declarations;
    built.generics=ctx->generics;
    built.types=ctx->types.count || ctx->types.nominals || ctx->types.interfaces?&ctx->types:NULL;
    return xr_xir_compile_check_v2(&ctx->compile,&built,ctx->construction,output,diagnostic);
}
