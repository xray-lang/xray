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
#include "xxir_type_match_internal.h"
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
static bool literal_outputs_empty(const XirLiteralDefinitions *output) {
    return output && !output->definition && !output->bindings && !output->count;
}
static XrXirStatus literal_result_frame(const XrXirCompileContext *context,
    const XirLiteralDefinitions *result,uint32_t count) {
    if(!result || !result->definition || !result->bindings || !count || result->count!=count)
        return XR_XIR_BAD_STRUCTURE;
    if(count>context->limits.functions) return XR_XIR_BUDGET;
    if((uint64_t)count>SIZE_MAX/sizeof(*result->bindings)) return XR_XIR_OVERFLOW;
    XrXirStatus status=xir_compile_resource_status(xr_compile_resources_allocation(context->resources,
        result->definition,sizeof(*result->definition)));
    if(status==XR_XIR_OK && result->definition->context.resources!=context->resources) return XR_XIR_BAD_STRUCTURE;
    if(status==XR_XIR_OK) status=xir_compile_resource_status(xr_compile_resources_allocation(context->resources,
        result->bindings,(size_t)count*sizeof(*result->bindings)));
    return status;
}
static XrXirStatus literal_arguments_frame(const XrXirCompileContext *context,const XirLiteralBinding *binding) {
    if(!binding->argument_count || !binding->arguments) return XR_XIR_BAD_STRUCTURE;
    if((uint64_t)binding->argument_count>SIZE_MAX/sizeof(*binding->arguments)) return XR_XIR_OVERFLOW;
    return xir_compile_resource_status(xr_compile_resources_allocation(context->resources,binding->arguments,
        (size_t)binding->argument_count*sizeof(*binding->arguments)));
}
static XrXirStatus literal_verify_bindings(const XrXirCompileContext *context,const XirCtfeInstanceView *views,
    const XirLiteralDefinitions *result,XirReadOperation *operation) {
    const XrXirModule *module=&result->definition->module;
    for(uint32_t i=0;i<result->count;++i) {
        const XirLiteralBinding *binding=&result->bindings[i];const XirCtfeInstanceView *view=&views[i];
        const XrXirOrigin *origin=&view->checked->module.provenance->origins[view->instance_function];
        XrXirStatus status=literal_arguments_frame(context,binding);
        if(status!=XR_XIR_OK) return status;
        if(binding->argument_count!=origin->argument_count || binding->definition!=view->definition_function ||
            binding->instance!=view->instance_function || binding->provider>=module->function_count)
            return XR_XIR_BAD_STRUCTURE;
        for(uint32_t a=0;a<binding->argument_count;++a) {
            const XirTypeMatchRead match={view->checked->module.types,module->types,NULL,0,
                origin->arguments[a],binding->arguments[a],&operation->capacity};
            status=xir_type_match_read(context,&match);if(status!=XR_XIR_OK) return status;
        }
        const XirTypeMatchRead match={view->lowered->module.types,module->types,NULL,0,
            (XrXirType)view->value->type,module->functions[binding->provider].result,&operation->capacity};
        status=xir_type_match_read(context,&match);if(status!=XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}
/* Source construction and its debug receiving have already completed. A
 * capacity retry repeats this entire pure relation, never their updates. */
static XrXirStatus literal_result_read(const XirLiteralRequest *request,const XrXirCompileContext *context,
    const XrXirComptimeSite *site,XirCtfeInstanceView *views,uint32_t count,
    const XirLiteralDefinitions *result,XirLiteralDefinitions *output,XirReadOperation *operation) {
    XirReadAttempt attempt={true};const XirReadRequest active={.operation=operation,.control=request->control,.attempt=&attempt};
    for(;;) {
        XrXirStatus status=xir_read_begin(context,operation);uint32_t progress=0;
        if(status==XR_XIR_OK && (request->context!=context || request->count!=count || !literal_outputs_empty(output)))
            status=XR_XIR_BAD_STRUCTURE;
        if(status==XR_XIR_OK) status=literal_result_frame(context,result,count);
        if(status==XR_XIR_OK) status=xir_artifact_verify(context,result->definition,NULL,active.control,&active);
        /* Only current storage geometry crosses these observation calls. No
         * earlier semantic match is carried into the callback-free tail. */
        for(uint32_t i=0;status==XR_XIR_OK && i<count;++i) {
            status=xir_receive_step(active.control,&progress);
            if(status==XR_XIR_OK) status=literal_result_frame(context,result,count);
            if(status!=XR_XIR_OK) break;
            status=literal_arguments_frame(context,&result->bindings[i]);
            uint32_t n=result->bindings[i].argument_count;
            for(uint32_t a=0;status==XR_XIR_OK && a<n;++a) {
                status=xir_receive_step(active.control,&progress);
                if(status==XR_XIR_OK) status=literal_result_frame(context,result,count);
                if(status==XR_XIR_OK) status=literal_arguments_frame(context,&result->bindings[i]);
                if(status==XR_XIR_OK && result->bindings[i].argument_count!=n) status=XR_XIR_BAD_STRUCTURE;
            }
        }
        if(status==XR_XIR_OK) status=xir_receive_poll(active.control);
        if(status==XR_XIR_OK && (request->context!=context || request->count!=count || !literal_outputs_empty(output)))
            status=XR_XIR_BAD_STRUCTURE;
        if(status==XR_XIR_OK) status=literal_result_frame(context,result,count);
        XirReadRequest fresh=active;fresh.control=NULL;
        if(status==XR_XIR_OK) status=xir_artifact_verify(context,result->definition,NULL,NULL,&fresh);
        if(status==XR_XIR_OK) {
            memset(views,0,(size_t)count*sizeof(*views));
            XrXirCtfeStatus queried=xir_ctfe_instance_batch_views_prepared(context,request->instances,views,count,&fresh);
            status=source_comptime_read_status(queried);
        }
        if(status==XR_XIR_OK) status=literal_site_verify(&views[0],site);
        if(status==XR_XIR_OK) status=literal_verify_bindings(context,views,result,operation);
        XirReadStep step=xir_read_step(operation,status);
        if(step.kind!=XIR_READ_NEED_PREPARE) return status;
        memset(views,0,(size_t)count*sizeof(*views));
        status=xir_read_prepare(context,operation,active.control);
        if(status!=XR_XIR_OK) return status;
    }
}
static XrXirStatus literal_receive(const XirLiteralRequest *request,const XrXirComptimeSite *site,
    XirLiteralDefinitions *output,XrXirDiagnostic *diagnostic,SourceContext *source_owner,XirReadOperation *read_operation) {
    if (!read_operation || !request || !xir_compile_context_valid(request->context) ||
        read_operation->capacity.resources!=request->context->resources || !request->instances || !request->count ||
        request->count>request->context->limits.functions || (site && request->count!=1) ||
        !output || output->definition || output->bindings || output->count)
        return literal_status(diagnostic,XR_XIR_BAD_STRUCTURE);
    const XrXirCompileContext *context=request->context;uint32_t count=request->count;
    XrXirStatus status=xir_receive_poll(request->control);
    XirCtfeInstanceView *views=xir_type_copy_zero(context,count,sizeof(*views),&status,request->control);
    if(!views) return literal_status(diagnostic,status);
    if(request->count!=count || request->context!=context) {
        xr_compile_resources_free(views);return literal_status(diagnostic,XR_XIR_BAD_STRUCTURE);
    }
    const XirReadRequest view_request={.operation=read_operation,.control=request->control};
    XrXirCtfeStatus queried=xir_ctfe_instance_batch_views_read(context,request->instances,views,count,&view_request);
    if (queried!=XR_XIR_CTFE_OK) {
        xr_compile_resources_free(views);
        return literal_status(diagnostic,queried==XR_XIR_CTFE_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:
            queried==XR_XIR_CTFE_COMPLEXITY_LIMIT?XR_XIR_BUDGET:
            queried==XR_XIR_CTFE_MEMORY_LIMIT?XR_XIR_MEMORY_LIMIT:
            queried==XR_XIR_CTFE_OVERFLOW?XR_XIR_OVERFLOW:
            queried==XR_XIR_CTFE_CANCELLED?XR_XIR_CANCELLED:
            queried==XR_XIR_CTFE_DEADLINE?XR_XIR_DEADLINE:
            queried==XR_XIR_CTFE_CLOCK_FAILURE?XR_XIR_CLOCK_FAILURE:XR_XIR_BAD_STRUCTURE);
    }
    SourceContext ctx={0};ctx.read_operation=read_operation;ctx.request=request->control;ctx.compile=*context;XirLiteralDefinitions result={0};
    const XrXirArtifact *original=views[0].definition;
    uint32_t progress=0;
    status=literal_site_verify(&views[0],site);
    if (status!=XR_XIR_OK) goto done;
    for (uint32_t i=0;i<count;++i) {
        status=xir_receive_step(request->control,&progress);if (status!=XR_XIR_OK) goto done;
        if (views[i].definition!=original || views[i].definition->context.resources!=context->resources) {
            status=XR_XIR_BAD_STRUCTURE;goto done;
        }
    }
    result.bindings=xir_type_copy_zero(context,count,sizeof(*result.bindings),&status,request->control);
    if (!result.bindings) goto done;
    result.count=count;
    if (!literal_seed(&ctx,original,count)) { status=ctx.diagnostic.status;goto done; }
    for (uint32_t i=0;i<count;++i) {
        status=xir_receive_step(request->control,&progress);if (status!=XR_XIR_OK) goto done;
        if (!literal_binding(&ctx,&views[i],&result.bindings[i]) || !literal_provider(&ctx,&views[i],&result.bindings[i])) {
            status=ctx.diagnostic.status;goto done;
        }
    }
    status=literal_check(&ctx,original,result.bindings,site,&result.definition,diagnostic);
    if (status==XR_XIR_OK && !source_generated_debug_receive(source_owner,&ctx,original->module.function_count))
        status=source_owner->diagnostic.status;
done:
    xr_hir_body_end(ctx.typed_body);
    source_comptime_context_free(&ctx,source_owner);
    if(status==XR_XIR_OK) status=literal_result_read(request,context,site,views,count,&result,output,read_operation);
    if(status==XR_XIR_OK) { *output=result;result=(XirLiteralDefinitions){0}; }
    xr_xir_compile_instance_literals_free(&result);xr_compile_resources_free(views);
    if (diagnostic) diagnostic->status=status;
    return status;
}
XR_FUNC XrXirStatus xr_xir_compile_instance_literals(const XirLiteralRequest *request,
    XirLiteralDefinitions *output,XrXirDiagnostic *diagnostic) {
    XirReadOperation read_operation={.capacity={.resources=request && request->context?request->context->resources:NULL}};
    XrXirStatus status=literal_receive(request,NULL,output,diagnostic,NULL,&read_operation);
    xir_read_operation_free(&read_operation);return status;
}
XR_FUNC XrXirStatus xr_xir_compile_instance_literals_at(const XirLiteralRequest *request,
    const XrXirComptimeSite *site,XirLiteralDefinitions *output,XrXirDiagnostic *diagnostic) {
    if(!site)return literal_status(diagnostic,XR_XIR_BAD_STRUCTURE);
    XirReadOperation read_operation={.capacity={.resources=request && request->context?request->context->resources:NULL}};
    XrXirStatus status=literal_receive(request,site,output,diagnostic,NULL,&read_operation);
    xir_read_operation_free(&read_operation);return status;
}
