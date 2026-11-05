/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_comptime_instances.inc.c - Closed results retain actual typed calls
 */
struct XirSourceComptimeOwner {
    XrXirCompileContext context;
    XirCtfeInstanceBatch *batch;
    XirSourceComptimeBinding *bindings;
    uint32_t count;
};
XR_FUNC void xr_xir_compile_source_comptime_instances_free(XirSourceComptimeInstances *result) {
    if (!result) return;
    XirSourceComptimeOwner *owner=result->owner;
    if (owner) {
        xr_xir_compile_ctfe_instance_batch_free(owner->batch);
        xr_compile_resources_free(owner->bindings);xr_compile_resources_free(owner);
    }
    *result=(XirSourceComptimeInstances){0};
}
static XrXirCtfeStatus source_comptime_binding_verify(
    const XirCtfeInstanceView *view,const XirSourceComptimeBinding *row) {

    const XrXirModule *closed=&view->checked->module,*source=&view->definition->module;
    const XrXirProvenance *proof=closed->provenance;
    if (row->source_function>=source->function_count || row->helper>=source->function_count ||
        row->function>=closed->function_count || row->callee>=closed->function_count ||
        row->callee!=view->instance_function || row->helper!=view->definition_function ||
        proof->origins[row->function].function!=row->source_function ||
        !source->generics || !source->generics[row->helper].parameter_count) return XR_XIR_CTFE_BAD_ARTIFACT;
    const XrXirFunction *a=&source->functions[row->source_function],*b=&closed->functions[row->function];
    if (row->source_instruction>=a->instruction_count || row->instruction>=b->instruction_count) return XR_XIR_CTFE_BAD_ARTIFACT;
    const XrXirInstruction *original=&a->instructions[row->source_instruction],*actual=&b->instructions[row->instruction];
    if ((original->op!=XR_XIR_CALL && original->op!=XR_XIR_INVOKE) || actual->op!=original->op ||
        original->immediate!=row->helper || actual->immediate!=row->callee || original->args[1] || actual->args[1] ||
        actual->type!=closed->functions[row->callee].result) return XR_XIR_CTFE_BAD_ARTIFACT;
    const XrXirOrigin *caller=&proof->origins[row->function],*callee=&proof->origins[row->callee];
    uint32_t count=source->generics[row->helper].parameter_count;
    if (caller->argument_count!=count || callee->argument_count!=count || original->type_arguments[1]!=count)
        return XR_XIR_CTFE_BAD_ARTIFACT;

    for (uint32_t t=0;t<count;++t) {
        if (caller->arguments[t]!=callee->arguments[t] ||
            source->generics[row->source_function].arguments[original->type_arguments[0]+t]!=(XrXirType)(XR_XIR_TYPE_PARAMETER_BASE+t)) return XR_XIR_CTFE_BAD_ARTIFACT;
    }
    return XR_XIR_CTFE_OK;
}
static XrXirCtfeStatus source_comptime_read_result(XrXirStatus status) {
    return status==XR_XIR_OK?XR_XIR_CTFE_OK:status==XR_XIR_OUT_OF_MEMORY?XR_XIR_CTFE_OUT_OF_MEMORY:
        status==XR_XIR_MEMORY_LIMIT?XR_XIR_CTFE_MEMORY_LIMIT:status==XR_XIR_OVERFLOW?XR_XIR_CTFE_OVERFLOW:
        status==XR_XIR_BUDGET?XR_XIR_CTFE_COMPLEXITY_LIMIT:status==XR_XIR_CANCELLED?XR_XIR_CTFE_CANCELLED:
        status==XR_XIR_DEADLINE?XR_XIR_CTFE_DEADLINE:status==XR_XIR_CLOCK_FAILURE?XR_XIR_CTFE_CLOCK_FAILURE:
        XR_XIR_CTFE_BAD_ARTIFACT;
}
static bool source_comptime_outputs_empty(const XirCtfeInstanceView *views,
    const XirSourceComptimeBinding *bindings,uint32_t count) {
    if(!views && !bindings) return true;
    if(!views || !bindings) return false;
    for(uint32_t i=0;i<count;++i) {
        const XirCtfeInstanceView *v=&views[i];const XirSourceComptimeBinding *b=&bindings[i];
        if(v->checked || v->lowered || v->definition || v->value || v->instance_function || v->definition_function ||
            b->source_function || b->source_instruction || b->helper || b->function || b->instruction || b->callee)
            return false;
    }
    return true;
}
static XrXirStatus source_comptime_result_frame(const XrXirCompileContext *context,
    const XirSourceComptimeInstances *result,uint32_t count) {
    if(!result || !result->owner || !count || count!=result->count) return XR_XIR_BAD_STRUCTURE;
    if(count>context->limits.functions) return XR_XIR_BUDGET;
    XrXirStatus status=xir_compile_resource_status(xr_compile_resources_allocation(
        context->resources,result->owner,sizeof(*result->owner)));
    if(status!=XR_XIR_OK) return status;
    const XirSourceComptimeOwner *owner=result->owner;
    if(owner->context.resources!=context->resources || owner->count!=count || !owner->batch || !owner->bindings)
        return XR_XIR_BAD_STRUCTURE;
    if((uint64_t)count>SIZE_MAX/sizeof(*owner->bindings)) return XR_XIR_OVERFLOW;
    return xir_compile_resource_status(xr_compile_resources_allocation(context->resources,owner->bindings,
        (size_t)count*sizeof(*owner->bindings)));
}
/* The opaque receiver uses exactly the same current view/binding relation for
 * standalone queries and the Source root after its child owners have retired. */
static XrXirStatus source_comptime_result_attempt(const XrXirCompileContext *context,
    const XirSourceComptimeInstances *result,XirCtfeInstanceView *views,XirSourceComptimeBinding *bindings,
    uint32_t count,const XirReadRequest *request,XrXirCtfeStatus *detail) {
    *detail=XR_XIR_CTFE_OK;
    XrXirStatus status=source_comptime_result_frame(context,result,count);
    if(status!=XR_XIR_OK) return status;
    if(!source_comptime_outputs_empty(views,bindings,count)) {*detail=XR_XIR_CTFE_BAD_ARGUMENT;return XR_XIR_BAD_STRUCTURE;}
    if((uint64_t)count>SIZE_MAX/sizeof(XirCtfeInstanceView)) return XR_XIR_OVERFLOW;
    XirReadClaim claim={0};
    XirCtfeInstanceView *received=xir_read_scratch_alloc(context,&request->operation->capacity,
        (size_t)count*sizeof(*received),true,&claim,&status);
    if(!received) return status;
    *detail=xir_ctfe_instance_batch_views_read(context,result->owner->batch,received,count,request);
    status=source_comptime_read_status(*detail);uint32_t progress=0;
    for(uint32_t i=0;status==XR_XIR_OK && i<count;++i) {
        status=xir_receive_step(request->control,&progress);
        if(status==XR_XIR_OK) status=source_comptime_result_frame(context,result,count);
        if(status==XR_XIR_OK) {
            *detail=source_comptime_binding_verify(&received[i],&result->owner->bindings[i]);
            status=source_comptime_read_status(*detail);
        }
    }
    if(status==XR_XIR_OK) status=xir_receive_poll(request->control);
    /* Reopening a view is not fresh: repeat the full packet, all rows and the
     * opaque Source binding relation after the last callback, without a free. */
    if(status==XR_XIR_OK) status=source_comptime_result_frame(context,result,count);
    if(status==XR_XIR_OK) {
        memset(received,0,(size_t)count*sizeof(*received));
        XirReadRequest fresh=*request;fresh.control=NULL;
        *detail=xir_ctfe_instance_batch_views_prepared(context,result->owner->batch,received,count,&fresh);
        status=source_comptime_read_status(*detail);
    }
    for(uint32_t i=0;status==XR_XIR_OK && i<count;++i) {
        *detail=source_comptime_binding_verify(&received[i],&result->owner->bindings[i]);
        status=source_comptime_read_status(*detail);
    }
    if(status==XR_XIR_OK && !source_comptime_outputs_empty(views,bindings,count)) {
        *detail=XR_XIR_CTFE_BAD_ARGUMENT;status=XR_XIR_BAD_STRUCTURE;
    }
    if(status==XR_XIR_OK && views) {
        memcpy(views,received,(size_t)count*sizeof(*views));
        memcpy(bindings,result->owner->bindings,(size_t)count*sizeof(*bindings));
    }
    xir_read_scratch_release(&request->operation->capacity,received,&claim);return status;
}
static XrXirStatus source_comptime_result_read(const XrXirCompileContext *context,
    const XirSourceComptimeInstances *result,XirCtfeInstanceView *views,XirSourceComptimeBinding *bindings,
    uint32_t count,const XirReadRequest *read_request,XrXirCtfeStatus *detail) {
    if(!read_request || !read_request->operation || !xir_read_request_matches(context,read_request))
        return XR_XIR_BAD_STRUCTURE;
    bool outer=!read_request->attempt;XirReadAttempt attempt={true};XirReadRequest request=*read_request;
    if(outer) request.attempt=&attempt;
    for(;;) {
        XrXirStatus status=outer?xir_read_begin(context,request.operation):XR_XIR_OK;
        *detail=XR_XIR_CTFE_OK;
        if(status==XR_XIR_OK) status=source_comptime_result_attempt(context,result,views,bindings,count,&request,detail);
        XirReadStep step=xir_read_step(request.operation,status);
        if(step.kind!=XIR_READ_NEED_PREPARE || !outer) return status;
        status=xir_read_prepare(context,request.operation,request.control);
        if(status!=XR_XIR_OK) {*detail=XR_XIR_CTFE_OK;return status;}
    }
}
XR_FUNC XrXirCtfeStatus xr_xir_compile_source_comptime_instances_view(const XirSourceComptimeInstances *result,
    XirCtfeInstanceView *views,XirSourceComptimeBinding *bindings,uint32_t count) {
    if(!result || !result->owner || !views || !bindings) return XR_XIR_CTFE_BAD_ARGUMENT;
    const XrXirCompileContext *context=&result->owner->context;
    XirReadOperation operation={.capacity={.resources=context->resources}};
    const XirReadRequest request={.operation=&operation};XrXirCtfeStatus detail=XR_XIR_CTFE_OK;
    XrXirStatus status=source_comptime_result_read(context,result,views,bindings,count,&request,&detail);
    xir_read_operation_free(&operation);
    return detail!=XR_XIR_CTFE_OK?detail:source_comptime_read_result(status);
}
static bool source_comptime_instances_collect(SourceContext *ctx,const XrXirArtifact *closed,
    XirSourceComptimeBinding *bindings,uint32_t *functions,uint32_t *count) {
    uint32_t progress=0;
    const XrXirModule *module=&closed->module;const XrXirProvenance *proof=module->provenance;
    if (!proof || proof->kind!=XR_XIR_EVIDENCE_INSTANCE || !proof->source || !proof->origins || proof->count!=module->function_count)
        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"comptime instances lost ordinary specialization provenance");
    for (uint32_t f=0;f<module->function_count;++f) {
        const XrXirFunction *body=&module->functions[f];
        for (const SourceComptimeSite *site=ctx->comptime_sites;site;site=site->next) {
            if (!source_receive_step(ctx,&progress)) return false;
            if (!ctx->generics[site->helper].parameter_count || proof->origins[f].function!=site->owner) continue;
            const XrXirFunction *original=&proof->source->module.functions[site->owner];uint32_t source_at=UINT32_MAX;
            for (uint32_t i=0;i<original->instruction_count;++i) {
                if (!source_receive_step(ctx,&progress)) return false;
                const XrXirInstruction *op=&original->instructions[i];
                if ((op->op==XR_XIR_CALL || op->op==XR_XIR_INVOKE) && op->immediate==site->helper) {
                    if (source_at!=UINT32_MAX) return source_fail(ctx,site->syntax,XR_XIR_BAD_STRUCTURE,"comptime source call is ambiguous");
                    source_at=i;
                }
            }
            if (source_at==UINT32_MAX) return source_fail(ctx,site->syntax,XR_XIR_BAD_STRUCTURE,"comptime source call is absent");
            uint32_t found=UINT32_MAX;
            for (uint32_t i=0;i<body->instruction_count;++i) {
                if (!source_receive_step(ctx,&progress)) return false;
                const XrXirInstruction *op=&body->instructions[i];
                if ((op->op!=XR_XIR_CALL && op->op!=XR_XIR_INVOKE) || op->immediate<0 || (uint64_t)op->immediate>=module->function_count ||
                    proof->origins[op->immediate].function!=site->helper) continue;
                if (found!=UINT32_MAX)
                    return source_fail(ctx,site->syntax,XR_XIR_BAD_STRUCTURE,"comptime collected call is ambiguous");
                if (*count>=ctx->compile.limits.functions)
                    return source_fail(ctx,site->syntax,XR_XIR_BUDGET,"comptime occurrence inventory exhausted");
                found=i;
                if ((bindings || functions) && !((ctx)->diagnostic.status == XR_XIR_OK)) return false;
                if (bindings) bindings[*count]=(XirSourceComptimeBinding){site->owner,source_at,site->helper,f,i,(uint32_t)op->immediate};
                if (functions) functions[*count]=(uint32_t)op->immediate;
                ++*count;
            }
            if (found==UINT32_MAX) return source_fail(ctx,site->syntax,XR_XIR_BAD_STRUCTURE,"comptime collected call is absent");
        }
    }
    return true;
}
static void source_comptime_instances_prepare(SourceContext *ctx,const XrXirArtifact *checked,
    const XirSourceComptimeRequest *request,XirSourceComptimeInstances *output) {
    if (ctx->diagnostic.status!=XR_XIR_OK) return;
    if (ctx->linkage_kind!=XR_XIR_PROGRAM || !ctx->comptime_count) {
        source_fail(ctx,NULL,XR_XIR_BAD_TYPE,"generic comptime staging requires actual Program occurrences");return;
    }
    XrXirArtifact *closed=NULL;XrXirStatus status=xr_xir_compile_specialize(checked,&closed,NULL,ctx->request);
    uint32_t count=0;
    if (status!=XR_XIR_OK) source_fail(ctx,NULL,status,"generic comptime specialization failed");
    if (ctx->diagnostic.status==XR_XIR_OK && source_comptime_instances_collect(ctx,closed,NULL,NULL,&count) && !count)
        source_fail(ctx,NULL,XR_XIR_BAD_TYPE,"no generic comptime call was actually collected");
    XirSourceComptimeInstances result={0};uint32_t *functions=NULL;
    if (ctx->diagnostic.status==XR_XIR_OK) {
        result.owner=xir_type_copy_zero(&ctx->compile,1,sizeof(*result.owner),&status,ctx->request);
        if (result.owner && ((ctx)->diagnostic.status == XR_XIR_OK)) {
            result.owner->context=ctx->compile;result.owner->count=count;result.count=count;
            result.owner->bindings=xir_type_copy_zero(&ctx->compile,count,sizeof(*result.owner->bindings),&status,ctx->request);
            if (status==XR_XIR_OK) functions=xir_type_copy_zero(&ctx->compile,count,sizeof(*functions),&status,ctx->request);
        }
        if (status!=XR_XIR_OK) source_fail(ctx,NULL,status,"generic comptime result storage failed");
    }
    uint32_t received=0;
    if (ctx->diagnostic.status==XR_XIR_OK && source_comptime_instances_collect(ctx,closed,result.owner->bindings,functions,&received)) {
        if (received!=count) source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"generic comptime occurrence coverage changed");
        else {
            XirCtfeInstanceBatchRequest evaluation={closed,functions,count,request->target,request->limits,request->call_depth};
            XrXirCtfeStatus evaluated=xr_xir_compile_ctfe_instance_batch(&evaluation,ctx->request,&result.owner->batch);
            if (evaluated!=XR_XIR_CTFE_OK) source_comptime_failure(ctx,NULL,evaluated);
        }
    }
    if (ctx->diagnostic.status==XR_XIR_OK) *output=result;
    else xr_xir_compile_source_comptime_instances_free(&result);
    xr_compile_resources_free(functions);xr_xir_compile_artifact_free(closed);
}
