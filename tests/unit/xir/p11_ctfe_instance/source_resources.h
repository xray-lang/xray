/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * source_resources.h - Whole Source receiving jobs retain one finite ledger
 */
typedef struct SourceReceivingFixture { uint32_t definition,functions[2]; } SourceReceivingFixture;
static SourceReceivingFixture source_receiving_fixture(const char *kind,unsigned ordinal) {
    XrXirCompileContext context;context_new(&context);XrXirArtifact *source=instance_source(&context,kind);
    const XrXirOrigin *a=&source->module.provenance->origins[instance_named(source,kind,ordinal)];
    const XrXirOrigin *b=&source->module.provenance->origins[instance_named(source,kind,1-ordinal)];
    CHECK(a->function==b->function && a->argument_count==1 && b->argument_count==1);
    if (!strcmp(kind,"nested")) CHECK(instance_argument_absent(source,a->arguments[0]) && instance_argument_absent(source,b->arguments[0]));
    SourceReceivingFixture result={a->function,{instance_named(source,kind,ordinal),instance_named(source,kind,1-ordinal)}};
    xr_xir_compile_artifact_free(source);xr_compile_resources_release(context.resources);
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);return result;
}
static XrXirStatus source_receiving_operation(XrXirCompileContext *context,const char *kind,
    const SourceReceivingFixture *fixture,XirSourceInstanceConstants *output) {
    XrCompilerSession *session=NULL;XrCompileResourceStatus created=xr_compile_session_new(context->resources,&session);
    if (created!=XR_COMPILE_RESOURCE_OK) return p11_resource_status(created);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"instance-constant",NULL};
    XrXirSourceRequest source={session,NULL,&authority,context,NULL,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceText text={NULL,instance_source_text(),strlen(instance_source_text())};
    XirSourceInstanceSelection selections[2]={{fixture->definition,fixture->functions[0]},{fixture->definition,fixture->functions[1]}};
    XirSourceInstanceRequest request={.source=&source,.text=&text,.selections=selections,.count=2,
        .target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},
        .limits={XR_XIR_CTFE_MAX_STEPS,XR_XIR_CTFE_MAX_FRAME_BYTES,XR_XIR_CTFE_MAX_OUTPUT_BYTES},.call_depth=XR_XIR_CTFE_MAX_CALL_DEPTH};
    XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=!strcmp(kind,"foreign")?instance_foreign_source(&source,NULL,&diagnostic,output,selections,2):
        xr_xir_compile_source_instance_constants(&request,output,&diagnostic);
    xr_compile_session_free(session);return status;
}
static InstanceCost source_receiving_cost(const char *kind,const SourceReceivingFixture *fixture,XrCompileResourceStats *complete) {
    XrXirCompileContext context;context_new(&context);XrCompileResourceStats before=stats(&context);
    size_t first=source_fixture_compile_attempts;XirSourceInstanceConstants result={0};
    CHECK(source_receiving_operation(&context,kind,fixture,&result)==XR_XIR_OK && result.count==2);
    XrCompileResourceStats after=stats(&context);
    InstanceCost cost={after.allocated_bytes-before.allocated_bytes,source_fixture_compile_attempts-first};
    xr_xir_compile_source_instance_constants_free(&result);if (complete) *complete=stats(&context);
    xr_compile_resources_release(context.resources);CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);return cost;
}
static void source_receiving_faults(const char *kind,unsigned ordinal,bool prefix) {
    SourceReceivingFixture fixture=source_receiving_fixture(kind,ordinal);InstanceCost cost=source_receiving_cost(kind,&fixture,NULL);
    size_t trials=prefix?2:cost.sites;
    for (size_t trial=0;trial<trials;++trial) {
        XrXirCompileContext context;context_new(&context);XrCompileResources *same=context.resources;
        XrCompileResourceStats before=stats(&context);size_t first=source_fixture_compile_attempts;
        size_t fault=prefix?(trial?cost.sites-1:0):trial;
        source_fixture_compile_injected=false;source_fixture_compile_fail_at=first+fault;XirSourceInstanceConstants result={0};
        XrXirStatus status=source_receiving_operation(&context,kind,&fixture,&result);
        if (status!=XR_XIR_OUT_OF_MEMORY) fprintf(stderr,"Source instance FI %s[%u] point=%zu/%zu status=%u\n",kind,ordinal,fault,cost.sites,status);
        CHECK(status==XR_XIR_OUT_OF_MEMORY && source_fixture_compile_injected && source_fixture_compile_attempts==first+fault+1);
        CHECK(!result.source.checked && !result.source.snapshot && !result.functions && !result.count && stats(&context).held_bytes==before.held_bytes);
        source_fixture_compile_fail_at=SIZE_MAX;
        status=source_receiving_operation(&context,kind,&fixture,&result);
        CHECK(status==XR_XIR_OK);
         {
            CHECK(result.count==2 && result.source.checked && result.source.snapshot && result.functions[0]!=result.functions[1]);
        }
        xr_xir_compile_source_instance_constants_free(&result);CHECK(context.resources==same && stats(&context).held_bytes==before.held_bytes);
        xr_compile_resources_release(context.resources);CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
        if (!(trial%256) || trial+1==trials) fprintf(stderr,"Source instance FI %s[%u] actual=%zu/%zu same original owner/retry/live physical0\n",kind,ordinal,fault+1,cost.sites);
    }
    fprintf(stderr,"Source instance %s[%u] %s actualFI=%zu selected=%zu\n",kind,ordinal,prefix?"FIRST_LAST_ONLY":"COMPLETE_ALL_ORDINALS",cost.sites,trials);
}
static void source_receiving_bounds(const char *kind,unsigned ordinal) {
    SourceReceivingFixture fixture=source_receiving_fixture(kind,ordinal);XrCompileResourceStats used={0};
    (void)source_receiving_cost(kind,&fixture,&used);
    for (unsigned minus=0;minus<2;++minus) {
        XrCompileResourceLimits limits=caps;
        limits.memory_limit=used.peak_bytes-minus;
        XrXirCompileContext context={0};CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
        context.limits=xr_xir_compile_default_limits();XrCompileResourceStats before=stats(&context);XirSourceInstanceConstants result={0};
        XrXirStatus status=source_receiving_operation(&context,kind,&fixture,&result);
        if (status!=(minus?XR_XIR_MEMORY_LIMIT:XR_XIR_OK)) fprintf(stderr,"Source instance bound %s[%u] minus=%u status=%u\n",kind,ordinal,minus,status);
        CHECK(status==(minus?XR_XIR_MEMORY_LIMIT:XR_XIR_OK));xr_xir_compile_source_instance_constants_free(&result);
        CHECK(stats(&context).held_bytes==before.held_bytes);
        if (minus) {
            CHECK(source_receiving_operation(&context,kind,&fixture,&result)==XR_XIR_MEMORY_LIMIT);
            CHECK(!result.source.checked && !result.functions && stats(&context).held_bytes==before.held_bytes);
        }
        xr_compile_resources_release(context.resources);CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    }
    fprintf(stderr,"Source instance bounds %s[%u] held exact/minus1 original-owner physical0 allocated=%llu peak=%llu\n",
        kind,ordinal,(unsigned long long)used.allocated_bytes,(unsigned long long)used.peak_bytes);
}
