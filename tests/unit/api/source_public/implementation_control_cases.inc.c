/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#include "../../xir/implementation_control_fixture.h"
static void implementation_execute(XrSourceArtifact *artifact,char *source,size_t length,DeclarationsCopyProbe *probe) {
    size_t polls=probe->polls;memset(source,'!',length);ArrayOutput output={0};
    XrSourceExecution *execution=execution_new();XrSourceCallOptions options;
    CHECK(xray_source_call_options_init(&options,sizeof(options))==XR_SOURCE_OK);
    options.context=&output;options.write=array_write;
    XrSourceInvocation invocation;CHECK(xray_source_invocation_init(&invocation,sizeof(invocation))==XR_SOURCE_OK);
    invocation.name="answer";invocation.name_length=6;XrSourceCall *call=NULL;
    CHECK(xray_source_invoke_begin(execution,artifact,&invocation,&options,&call)==XR_SOURCE_OK&&call);
    xray_source_artifact_free(artifact);xray_source_execution_free(execution);
    CHECK(finish(call).state==XR_SOURCE_RUN_RETURNED);XrSourceResult *result=NULL;
    CHECK(xray_source_call_take(call,&result)==XR_SOURCE_OK&&result);
    CHECK(xray_source_call_drop(&call,NULL)==XR_SOURCE_OK&&!call);
    int64_t value=0;CHECK(xray_source_result_i64(result,&value)==XR_SOURCE_OK&&value==42);
    CHECK(output.size==3&&!memcmp(output.bytes,"42\n",3)&&probe->polls==polls);xray_source_result_free(result);
}
static bool implementation_raw_poll(const void *context) {
    DeclarationsCopyProbe *probe=(DeclarationsCopyProbe *)context;
    if(probe->destination&&!probe->wanted&&!memcmp(probe->destination,probe->argument_source,4096)) {
        for(size_t i=4096;i<probe->argument_bytes;++i)CHECK(probe->destination[i]==0xa5);
        probe->wanted=true;
    }
    return declarations_poll(context)!=0;
}
static void implementation_argument_controls(void) {
    XrXirType arguments[4096];for(size_t i=0;i<4096;++i)arguments[i]=XR_XIR_I64;
    XrXirImplementation record={.interface={0,arguments,4096}};
    XrXirImplementationTable source={&record,1};
    for(unsigned mode=0;mode<2;++mode) {
        XrCompileResources *resources=sdk_ledger(&sdk_unlimited);
        XrXirCompileContext context={resources,xr_xir_compile_default_limits()};
        uint64_t held=sdk_stats(resources).held_bytes;
        DeclarationsCopyProbe probe={.phase=IC_ARGUMENT,.mode=mode,.implementation=true};
        XrCompileRequestOptions options={.cancel_poll=implementation_raw_poll,.cancel_context=&probe};
        if(mode==1){options.deadline_mode=XR_COMPILE_DEADLINE_AFTER;options.timeout_ns=UINT64_C(100000000);}
        XrCompileRequestControl control={0};
        CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
        XrXirImplementationTable *copy=(void *)(uintptr_t)1;
        test_declarations_copy_begin(&probe);
        XrXirStatus status=test_implementation_copy(&context,&source,&control,&copy);
        test_declarations_copy_end(&probe);
        CHECK(status==(mode==0?XR_XIR_CANCELLED:XR_XIR_DEADLINE)&&copy==(void *)(uintptr_t)1);
        CHECK(probe.witnesses==1&&probe.argument_bytes==16384&&probe.copies==1);
        CHECK(sdk_stats(resources).held_bytes==held);
        CHECK(xr_compile_request_control_poll(&control)==(mode==0?XR_COMPILE_RESOURCE_CANCELLED:XR_COMPILE_RESOURCE_DEADLINE));
        control=(XrCompileRequestControl){0};CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
        probe=(DeclarationsCopyProbe){.phase=IC_ARGUMENT,.mode=3,.implementation=true};
        options.cancel_context=&probe;control.cancel_context=&probe;copy=NULL;
        test_declarations_copy_begin(&probe);CHECK(test_implementation_copy(&context,&source,&control,&copy)==XR_XIR_OK);
        test_declarations_copy_end(&probe);
        CHECK(copy&&copy->records!=&record&&copy->records[0].interface.arguments!=arguments);
        CHECK(!memcmp(copy->records[0].interface.arguments,arguments,sizeof(arguments)));
        xr_xir_compile_implementations_free(copy);CHECK(sdk_stats(resources).held_bytes==held);
        xr_compile_resources_release(resources);CHECK(!runtime_live&&!runtime_bytes);
        fprintf(stderr,"implementation raw4096args/16384B mode%u prefix4096 tail12288 rollback and same-options retry PASS\n",mode);
    }
}
static void implementation_controls(const char *selected) {
    if(selected&&!strcmp(selected,"raw_arguments")){implementation_argument_controls();return;}
    static const struct {const char *name;unsigned phase,mode;} cases[]={
        {"baseline",IC_RECORD,3},{"cancel_record",IC_RECORD,0},{"cancel_binding",IC_BINDING,0},
        {"cancel_publication",IC_PUBLICATION,0},{"deadline_publication",IC_PUBLICATION,1},
        {"oom_binding",IC_BINDING,2}};
    unsigned choice=0;
    if(selected){while(choice<sizeof(cases)/sizeof(*cases)&&strcmp(selected,cases[choice].name))++choice;CHECK(choice<sizeof(cases)/sizeof(*cases));}
    char text[8000];size_t length=implementation_control_fixture(text,sizeof(text));
    XrSourceInput input={.kind=XR_SOURCE_INPUT_TEXT,.value.text={NULL,"implementation-copy",text,length}};
    uint64_t baseline_ns=0;
    for(unsigned n=0;n<sizeof(cases)/sizeof(*cases);++n) {
        if(selected&&n!=choice&&!(cases[choice].mode==1&&n==0))continue;
        DeclarationsCopyProbe probe={.phase=cases[n].phase,.mode=cases[n].mode,.implementation=true,
            .capture=declarations_capture,.fail_next=nominal_fail_next};
        XrSourceCompileOptions options=xray_source_compile_options_default();options.stdlib_path=XR_PUBLIC_STDLIB;
        options.cancel_poll=declarations_poll;options.cancel_context=&probe;
        if(probe.mode==1){options.deadline_mode=XR_SOURCE_DEADLINE_AFTER;options.timeout_ns=baseline_ns+UINT64_C(5000000000);}
        XrSourceArtifact *artifact=NULL;XrSourceDiagnostic *report=NULL;XrSourceFailure failure={0};Calibration request={0};
        test_declarations_copy_begin(&probe);uint64_t start=xr_time_monotonic_ns();CHECK(start);
        XrSourceStatus status=calibration_build(&input,&options,&request,&artifact,&failure,&report);
        uint64_t elapsed=xr_time_monotonic_ns()-start;test_declarations_copy_end(&probe);runtime_fail_at=SIZE_MAX;
        fprintf(stderr,"implementation %s status%u witnesses%u oom%u helperHeld%llu/%llu declarationHeld%llu/%llu elapsedNs%llu\n",
            cases[n].name,(unsigned)status,probe.witnesses,probe.oom_seen?1u:0u,
            (unsigned long long)probe.implementation_held_before,(unsigned long long)probe.implementation_held_after,
            (unsigned long long)probe.held_before,(unsigned long long)probe.held_after,(unsigned long long)elapsed);
        if(probe.mode==3) {
            CHECK(status==XR_SOURCE_OK&&artifact&&!report&&probe.resources&&probe.implementations_returned);
            baseline_ns=elapsed;implementation_execute(artifact,text,length,&probe);
        } else {
            XrSourceStatus expected=probe.mode==0?XR_SOURCE_CANCELLED:probe.mode==1?XR_SOURCE_DEADLINE:XR_SOURCE_OUT_OF_MEMORY;
            CHECK(status==expected&&!artifact&&failure.status==expected&&failure.cleanup_status==XR_SOURCE_OK&&failure.stage==XR_SOURCE_STAGE_COMPILE);
            CHECK(probe.mode==2?probe.oom_seen:probe.witnesses==1);
            CHECK(probe.implementation_held_before&&probe.implementation_held_before==probe.implementation_held_after);
            xray_source_diagnostic_free(report);report=NULL;
            CHECK(probe.resources&&sdk_stats(probe.resources).held_bytes==probe.held_empty);
            xr_compile_resources_release(probe.resources);probe.resources=NULL;CHECK(!runtime_live&&!runtime_bytes);
            CHECK(xray_source_build(&input,&options,&artifact,&failure,NULL)==XR_SOURCE_OK&&artifact);
            implementation_execute(artifact,text,length,&probe);
        }
        if(probe.resources){CHECK(sdk_stats(probe.resources).held_bytes==probe.held_empty);xr_compile_resources_release(probe.resources);}
        CHECK(!runtime_live&&!runtime_bytes);length=implementation_control_fixture(text,sizeof(text));
    }
    if(!selected)implementation_argument_controls();
    printf("Complete7658B generic implementation: case=%s, request retirement, ready named answer42 and release PASS\n",selected?selected:"all");
}
