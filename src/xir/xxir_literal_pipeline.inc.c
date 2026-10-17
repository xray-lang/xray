/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_literal_pipeline.inc.c - Atomic receiving from verified instance owners
 *
 * KEY CONCEPT:
 *   Every result is freshly verified before an AST-independent writer borrows it.
 *   Final ordinary checking owns all definitions, types, strings and bindings.
 */
#include "xxir_literal_seed.inc.c"
#include "xxir_literal_sites.inc.c"
#include "xxir_literal_writer.inc.c"
static XrXirStatus literal_status(XrXirDiagnostic *diagnostic,XrXirStatus status) {
    if (diagnostic) diagnostic->status=status;
    return status;
}
XR_FUNC void xr_xir_compile_instance_literals_free(XirLiteralDefinitions *result) {
    if (!result) return;
    for (uint32_t i=0;result->bindings && i<result->count;++i)
        xr_compile_resources_free((void *)result->bindings[i].arguments);
    xr_compile_resources_free(result->bindings);xr_xir_compile_artifact_free(result->definition);
    *result=(XirLiteralDefinitions){0};
}
static XrXirStatus literal_verify_bindings(const XrXirCompileContext *context,const XirCtfeInstanceView *views,
    const XirLiteralDefinitions *result) {
    const XrXirModule *module=&result->definition->module;
    for (uint32_t i=0;i<result->count;++i) {
        const XirLiteralBinding *binding=&result->bindings[i];const XirCtfeInstanceView *view=&views[i];
        const XrXirOrigin *origin=&view->checked->module.provenance->origins[view->instance_function];
        for (uint32_t a=0;a<binding->argument_count;++a) {
            XrXirStatus status=xr_xir_compile_type_substitution_matches_between(context,view->checked->module.types,module->types,
                NULL,0,origin->arguments[a],binding->arguments[a]);
            if (status!=XR_XIR_OK) return status;
        }
        XrXirStatus status=xr_xir_compile_type_substitution_matches_between(context,view->lowered->module.types,module->types,
            NULL,0,(XrXirType)view->value->type,module->functions[binding->provider].result);
        if (status!=XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}
static XrXirStatus literal_receive(const XirLiteralRequest *request,const XrXirComptimeSite *site,
    XirLiteralDefinitions *output,XrXirDiagnostic *diagnostic) {
    if (!request || !xir_compile_context_valid(request->context) || !request->instances || !request->count ||
        request->count>request->context->limits.functions || (site && request->count!=1) ||
        !output || output->definition || output->bindings || output->count)
        return literal_status(diagnostic,XR_XIR_BAD_STRUCTURE);
    const XrXirCompileContext *context=request->context;XrXirStatus status=XR_XIR_OK;
    XirCtfeInstanceView *views=xir_compile_calloc(context,request->count,sizeof(*views),&status);
    if (!views) return literal_status(diagnostic,status);
    XrXirCtfeStatus queried=xr_xir_compile_ctfe_instance_batch_views(request->instances,views,request->count);
    if (queried!=XR_XIR_CTFE_OK) {
        xr_compile_resources_free(views);
        return literal_status(diagnostic,queried==XR_XIR_CTFE_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:
            queried==XR_XIR_CTFE_BUDGET?XR_XIR_BUDGET:XR_XIR_BAD_STRUCTURE);
    }
    SourceContext ctx={0};ctx.compile=*context;XirLiteralDefinitions result={0};
    const XrXirArtifact *original=views[0].definition;
    status=literal_site_verify(context,&views[0],site);
    if (status!=XR_XIR_OK) goto done;
    for (uint32_t i=0;i<request->count;++i) {
        if (!xir_compile_work(context,sizeof(*views))) { status=XR_XIR_BUDGET;goto done; }
        if (views[i].definition!=original || views[i].definition->context.resources!=context->resources) {
            status=XR_XIR_BAD_STRUCTURE;goto done;
        }
    }
    result.bindings=xir_compile_calloc(context,request->count,sizeof(*result.bindings),&status);
    if (!result.bindings) goto done;
    result.count=request->count;
    if (!literal_seed(&ctx,original,request->count)) { status=ctx.diagnostic.status;goto done; }
    for (uint32_t i=0;i<request->count;++i) {
        if (!literal_binding(&ctx,&views[i],&result.bindings[i]) || !literal_provider(&ctx,&views[i],&result.bindings[i])) {
            status=ctx.diagnostic.status;goto done;
        }
    }
    status=literal_check(&ctx,original,result.bindings,site,&result.definition,diagnostic);
    if (status==XR_XIR_OK) status=literal_verify_bindings(context,views,&result);
    if (status==XR_XIR_OK) { *output=result;result=(XirLiteralDefinitions){0}; }
done:
    xr_xir_compile_instance_literals_free(&result);source_comptime_context_free(&ctx);xr_compile_resources_free(views);
    if (diagnostic) diagnostic->status=status;
    return status;
}
XR_FUNC XrXirStatus xr_xir_compile_instance_literals(const XirLiteralRequest *request,
    XirLiteralDefinitions *output,XrXirDiagnostic *diagnostic) {
    return literal_receive(request,NULL,output,diagnostic);
}
XR_FUNC XrXirStatus xr_xir_compile_instance_literals_at(const XirLiteralRequest *request,
    const XrXirComptimeSite *site,XirLiteralDefinitions *output,XrXirDiagnostic *diagnostic) {
    return site?literal_receive(request,site,output,diagnostic):literal_status(diagnostic,XR_XIR_BAD_STRUCTURE);
}
