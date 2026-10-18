/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_comptime_library.inc.c - Ordinary Program birth for library CTFE
 *
 * KEY CONCEPT:
 *   Library templates are never relabelled executable. The same immutable
 *   graph independently constructs a real Program on the original ledger.
 */
static bool source_comptime_library_results(SourceContext *ctx,const SourceContext *program) {
    if (ctx->comptime_count!=program->comptime_count)
        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"library evaluation occurrence inventory changed");
    for (SourceComptimeSite *site=ctx->comptime_sites;site;site=site->next) {
        const SourceComptimeSite *matched=NULL;
        for (const SourceComptimeSite *candidate=program->comptime_sites;candidate;candidate=candidate->next) {
            if (!source_work_units(ctx,site->syntax,sizeof(*candidate))) return false;
            if (candidate->syntax!=site->syntax || candidate->module!=site->module) continue;
            const SourceFunction *a=&ctx->bodies[site->owner],*b=&program->bodies[candidate->owner];
            const XrXirFunctionIdentity *x=&ctx->identities[site->owner],*y=&program->identities[candidate->owner];
            if (a->node!=b->node || a->module!=b->module || x->nominal_owner!=y->nominal_owner ||
                x->method_kind!=y->method_kind || matched)
                return source_fail(ctx,site->syntax,XR_XIR_BAD_STRUCTURE,"library evaluation declaration ancestry changed");
            matched=candidate;
        }
        if (!matched) return source_fail(ctx,site->syntax,XR_XIR_BAD_STRUCTURE,"library evaluation occurrence is absent");
        uint32_t count=ctx->generics[site->helper].parameter_count;
        if (program->generics[matched->helper].parameter_count!=count)
            return source_fail(ctx,site->syntax,XR_XIR_BAD_STRUCTURE,"library symbolic binder inventory changed");
        XrXirType *arguments=count?source_alloc(ctx,count,sizeof(*arguments)):NULL;
        if (count && !arguments) return false;
        for (uint32_t a=0;a<count;++a) {
            if (!source_work_units(ctx,site->syntax,sizeof(*arguments))) return false;
            arguments[a]=(XrXirType)(XR_XIR_TYPE_PARAMETER_BASE+a);
        }
        if (matched->expected.present!=site->expected.present || matched->expected.infer_result!=site->expected.infer_result ||
            matched->expected.inferred!=site->expected.inferred)
            return source_fail(ctx,site->syntax,XR_XIR_BAD_STRUCTURE,"library evaluation expected context changed");
        if (site->expected.present) {
            XrXirStatus expected=xr_xir_compile_type_substitution_matches_between(&ctx->compile,
                &program->types,&ctx->types,arguments,count,matched->expected.type,site->expected.type);
            if (expected!=XR_XIR_OK) return source_fail(ctx,site->syntax,expected,"library evaluation expected type changed");
        }
        XrXirStatus status=xr_xir_compile_type_substitution_matches_between(&ctx->compile,
            &program->types,&ctx->types,arguments,count,program->functions[matched->helper].result,ctx->functions[site->helper].result);
        if (status!=XR_XIR_OK) return source_fail(ctx,site->syntax,status,"library evaluation result type changed");
        if (count) continue;
        if (!source_comptime_result_value(ctx,site->syntax,&matched->value,matched->result_arity,matched->result_work))
            return source_fail(ctx,site->syntax,ctx->diagnostic.status==XR_XIR_OK?XR_XIR_BAD_STRUCTURE:ctx->diagnostic.status,
                "library evaluation result is not canonical");
        if (!source_work_units(ctx,site->syntax,sizeof(site->value)+sizeof(site->result_arity)+sizeof(site->result_work))) return false;
        XrXirValueStatus copied=xr_xir_value_copy(&matched->value,&site->value);
        if (copied!=XR_XIR_VALUE_OK) return source_fail(ctx,site->syntax,
            copied==XR_XIR_VALUE_REFCOUNT_LIMIT || copied==XR_XIR_VALUE_LIMIT?XR_XIR_BUDGET:XR_XIR_BAD_STRUCTURE,
            "library evaluation value receiving failed");
        site->result_arity=matched->result_arity;site->result_work=matched->result_work;
    }
    return true;
}
static bool source_comptime_evaluate_library(SourceContext *ctx,const XrXirArtifact *checked) {
    XrXirStatus status=xr_xir_compile_artifact_verify(checked,NULL);
    if (status!=XR_XIR_OK) return source_fail(ctx,NULL,status,"library CTFE input failed current checking");
    if (!source_work_units(ctx,NULL,sizeof(SourceContext))) return false;
    SourceContext program={0};program.compile=ctx->compile;program.graph=ctx->graph;program.linkage_kind=XR_XIR_PROGRAM;
    program.manifests=ctx->manifests;program.prelude_module=ctx->prelude_module;program.core_factory=ctx->core_factory;
    program.remaining_blocks=ctx->remaining_blocks;program.remaining_instructions=ctx->remaining_instructions;
    XrXirSourceResult output={0};source_construct(&program,&output);
    XrXirArtifact *complete=NULL;
    bool ok=program.diagnostic.status==XR_XIR_OK && source_program_complete(&program,output.checked,&complete) &&
        source_comptime_evaluate(&program,complete,true);
    ctx->remaining_blocks=program.remaining_blocks;ctx->remaining_instructions=program.remaining_instructions;
    if (ok) ok=source_comptime_library_results(ctx,&program);
    else { ctx->diagnostic=program.diagnostic;ctx->module=program.module; }
    xr_xir_compile_artifact_free(complete);xr_xir_compile_source_result_free(&output);source_comptime_context_free(&program);return ok;
}
