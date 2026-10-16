/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_handlers.c - Actual typed and pattern catch acceptance sites
 */
#define main reference_handler_launch_main
#include "test_launch.c"
#undef main
#include "xir/xxir_output.h"
#include "xir/xxir_source_query_internal.h"

typedef struct HandlerCase {
    const char *text,*output,*stops[4];
    uint32_t sites,count;
} HandlerCase;
typedef struct HandlerFixture {
    XrCompileResources *resources;
    XrSourceCompiler *compiler;
    XrXirDomain *domain;
    XrCompileResourceStats baseline;
    XrSourceCompileDiagnostic diagnostic;
    XdapLaunch *launch;
    XrXirOutputSink sink;
    char output[128];
    size_t length;
} HandlerFixture;
static XrXirOutputStatus handler_output(void *context,XrXirOutputStream stream,const char *bytes,size_t length) {
    HandlerFixture *fixture=context;
    CHECK(stream==XR_XIR_STDOUT && length<sizeof(fixture->output)-fixture->length);
    memcpy(fixture->output+fixture->length,bytes,length);fixture->length+=length;return XR_XIR_OUTPUT_OK;
}
static void handler_open(HandlerFixture *fixture) {
    XrCompileResourceLimits limits={67108864,8388608,128000000};
    CHECK(xr_compile_resources_new(&limits,&fixture->resources)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_resources_stats(fixture->resources,&fixture->baseline)==XR_COMPILE_RESOURCE_OK);
    XrSourceCompilerConfig source={{fixture->resources,xr_xir_compile_default_limits()},
        {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},XR_DAP_STDLIB,xr_source_compile_default_manifest_limits()};
    CHECK(xr_source_compiler_new(&source,&fixture->compiler,&fixture->diagnostic)==XR_SOURCE_COMPILE_OK);
    xr_source_compile_diagnostic_free(&fixture->diagnostic);
    XrXirDomainBudgetControls budget={1048576,1048576,128000000,1048576,1048576};
    CHECK(xr_xir_domain_new_budgeted(1048576,&budget,&fixture->domain)==XR_XIR_VALUE_OK);
}
static XrXirStatus handler_build(HandlerFixture *fixture,const char *text) {
    XrSourceCompilerText input={NULL,"dap-handler-sites",text,strlen(text)};
    XdapLaunchRequest request={.compiler=fixture->compiler,.text=&input,.observer_metadata=fixture->domain};
    CHECK(xr_xir_instance_config_init(&request.instance,sizeof(request.instance))==XR_XIR_CALL_READY);
    request.instance.metadata_limit=request.instance.value_limit=request.instance.call_limit=1048576;
    request.instance.requested_value_limit=request.instance.requested_call_limit=1048576;
    request.instance.work_limit=128000000;request.instance.poll_limit=10000;request.instance.depth_limit=64;
    fixture->sink=(XrXirOutputSink){XR_XIR_CALL_ABI_VERSION,0,handler_output,fixture,sizeof(fixture->output)};
    request.instance.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,xr_xir_output_render,&fixture->sink};
    return xdap_launch_build(&request,&fixture->launch,&fixture->diagnostic);
}
static void handler_close(HandlerFixture *fixture) {
    CHECK(xdap_launch_free(fixture->launch)==XR_XIR_CALL_READY);fixture->launch=NULL;
    xr_source_compile_diagnostic_free(&fixture->diagnostic);xr_source_compiler_free(fixture->compiler);
    CHECK(!xr_xir_domain_budget_stats(fixture->domain).metadata_live);xr_xir_domain_drop(fixture->domain);
    XrCompileResourceStats after={0};
    CHECK(xr_compile_resources_stats(fixture->resources,&after)==XR_COMPILE_RESOURCE_OK);
    CHECK(after.live_bytes==fixture->baseline.live_bytes);xr_compile_resources_release(fixture->resources);
    CHECK(!runtime_live && !runtime_bytes);
}
static uint32_t handler_line(const char *text,const char *marker) {
    const char *found=strstr(text,marker);CHECK(found && !strstr(found+strlen(marker),marker));
    uint32_t line=1;for (const char *at=text;at<found;++at) if (*at=='\n') ++line;return line;
}
static XrXirSourceSnapshot *handler_publication(HandlerFixture *fixture,const XrXirSourceDebugView *debug,
    XrXirVmDebugLocation location) {
    const XrXirCompileContext *context=xr_source_compiler_context(fixture->compiler);
    XrXirSourceProductPacketView packet={0};XrXirArtifact *checked=NULL;
    CHECK(xr_xir_compile_source_product_packet(fixture->launch->source,XR_XIR_SOURCE_PRODUCT_CLOSED,&packet)==XR_XIR_OK);
    CHECK(xr_xir_compile_checked_read(context,packet.bytes,packet.length,&checked,NULL)==XR_XIR_OK);
    XrXirSourceSnapshot *snapshot=NULL;
    CHECK(xr_xir_compile_source_snapshot_copy_v2(context,xr_xir_compile_source_product_view(fixture->launch->source),
        xr_xir_compile_source_product_construction(fixture->launch->source),&snapshot)==XR_XIR_OK);
    XrXirSourceDebugFunction *functions=NULL;XrXirSourceDebugPoint *points=NULL;
    const XrXirSourceDebugFunction *original=&debug->functions[location.function];
    size_t function_bytes=(size_t)debug->function_count*sizeof(*functions),point_bytes=(size_t)original->instruction_count*sizeof(*points);
    CHECK(xr_compile_resources_alloc(fixture->resources,function_bytes,(void **)&functions)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_resources_alloc(fixture->resources,point_bytes,(void **)&points)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_resources_work(fixture->resources,function_bytes+point_bytes)==XR_COMPILE_RESOURCE_OK);
    memcpy(functions,debug->functions,function_bytes);memcpy(points,original->points,point_bytes);
    functions[location.function].points=points;XrXirSourceDebugView candidate=*debug;candidate.functions=functions;
    XrXirSourceDebugPoint preserved=points[location.instruction];
    const XrXirModule *module=xr_xir_compile_artifact_module(checked);
    XrCompileResourceStats before={0},after={0};CHECK(xr_compile_resources_stats(fixture->resources,&before)==XR_COMPILE_RESOURCE_OK);
    for (uint32_t bad=0;bad<4;++bad) {
        points[location.instruction]=preserved;
        if (bad==0) points[location.instruction].flags|=4u;
        else if (bad==1) points[location.instruction].statement=0;
        else if (bad==2) points[location.instruction].range.line=0;
        else points[location.instruction].environment=original->binding_count+1;
        CHECK(xr_xir_compile_source_snapshot_debug_copy(snapshot,&candidate,module)==XR_XIR_BAD_STRUCTURE);
        CHECK(!xr_xir_compile_source_snapshot_debug(snapshot));
        CHECK(xr_compile_resources_stats(fixture->resources,&after)==XR_COMPILE_RESOURCE_OK);
        CHECK(after.live_bytes==before.live_bytes && after.work>before.work);
    }
    points[location.instruction]=preserved;
    CHECK(xr_xir_compile_source_snapshot_debug_copy(snapshot,&candidate,module)==XR_XIR_OK);
    const XrXirSourceDebugView *owned=xr_xir_compile_source_snapshot_debug(snapshot);
    CHECK(owned && owned->functions!=functions && owned->functions[location.function].points!=points);
    memset(points,0,point_bytes);memset(functions,0,function_bytes);
    CHECK(!memcmp(&owned->functions[location.function].points[location.instruction],&preserved,sizeof(preserved)));
    xr_compile_resources_free(points);xr_compile_resources_free(functions);xr_xir_compile_artifact_free(checked);return snapshot;
}
static void handler_case(const HandlerCase *expected) {
    HandlerFixture fixture={0};handler_open(&fixture);
    XrXirStatus status=handler_build(&fixture,expected->text);
    if (status!=XR_XIR_OK) fprintf(stderr,"handler source=%u stage=%u: %s\n%s\n",status,
        fixture.diagnostic.source.stage,fixture.diagnostic.source.source.message,expected->text);
    CHECK(status==XR_XIR_OK && fixture.launch);
    const XrXirSourceDebugView *debug=xr_xir_compile_source_product_debug(fixture.launch->source);CHECK(debug);
    XrXirVmDebugLocation sites[32];uint32_t count=0;
    for (uint32_t f=0;f<debug->function_count;++f) {
        const XrXirSourceDebugFunction *function=&debug->functions[f];
        for (uint32_t i=0;i<function->instruction_count;++i) {
            const XrXirSourceDebugPoint *point=&function->points[i];
            if (!(point->flags & XR_XIR_SOURCE_DEBUG_HANDLER_ACCEPT)) continue;
            CHECK(point->statement && point->range.line>0 && point->range.column>0 && count<32);
            sites[count++]=(XrXirVmDebugLocation){f,i};
            XrXirSourceDebugPoint copied={0};
            CHECK(xdap_launch_source_point(fixture.launch,f,i,&copied)==XR_XIR_OK && !memcmp(&copied,point,sizeof(copied)));
        }
    }
    if (count!=expected->sites) fprintf(stderr,"handler site count expected=%u actual=%u\n",expected->sites,count);
    CHECK(count==expected->sites);
    XrXirSourceSnapshot *publication=count ? handler_publication(&fixture,debug,sites[0]) : NULL;
    CHECK(xdap_current_breakpoints_replace(fixture.launch->control,sites,count)==XR_XIR_VALUE_OK);
    xr_source_compiler_free(fixture.compiler);fixture.compiler=NULL;
    CHECK(xdap_current_start(fixture.launch->control,fixture.launch->entry,NULL,0)==XR_XIR_CALL_READY);
    XrXirVmDebugSnapshot *escaped=NULL;uint32_t stopped=0;
    for (uint32_t polls=0;polls<64;++polls) {
        XdapCurrentResult result={0};CHECK(xdap_current_poll(fixture.launch->control,1000,&result)==XR_XIR_VALUE_OK);
        if (result.state==XDAP_CURRENT_STOPPED) {
            CHECK(stopped<expected->count);
            XrXirVmDebugSnapshot *snapshot=NULL;CHECK(xdap_current_snapshot(fixture.launch->control,&snapshot)==XR_XIR_VALUE_OK);
            XrXirVmDebugFrameInfo frame={0};XrXirValue name={0};
            CHECK(xr_xir_vm_debug_snapshot_frame(snapshot,0,&frame,&name)==XR_XIR_VALUE_OK);xr_xir_value_drop(&name);
            XrXirSourceDebugPoint point={0};CHECK(xdap_launch_source_point(fixture.launch,frame.function,frame.instruction,&point)==XR_XIR_OK);
            CHECK(point.flags & XR_XIR_SOURCE_DEBUG_HANDLER_ACCEPT);
            CHECK((uint32_t)point.range.line==handler_line(expected->text,expected->stops[stopped]));
            ++stopped;
            if (!escaped) escaped=snapshot;else xr_xir_vm_debug_snapshot_drop(snapshot);
            CHECK(xdap_current_continue(fixture.launch->control,result.debug_epoch,XR_XIR_VM_DEBUG_CONTINUE)==XR_XIR_VALUE_OK);
        } else if (result.state==XDAP_CURRENT_ENDED) {
            CHECK(result.outcome.status==XR_XIR_CALL_RETURNED);xdap_current_result_drop(&result);break;
        }
        xdap_current_result_drop(&result);CHECK(polls<63);
    }
    CHECK(stopped==expected->count && fixture.length==strlen(expected->output));
    CHECK(!memcmp(fixture.output,expected->output,fixture.length));
    CHECK(xdap_launch_free(fixture.launch)==XR_XIR_CALL_READY);fixture.launch=NULL;
    if (publication) {
        const XrXirSourceDebugView *owned=xr_xir_compile_source_snapshot_debug(publication);CHECK(owned);
        CHECK(owned->functions[sites[0].function].points[sites[0].instruction].flags & XR_XIR_SOURCE_DEBUG_HANDLER_ACCEPT);
        xr_xir_compile_source_snapshot_free(publication);
    }
    if (escaped) {
        XrXirVmDebugFrameInfo frame={0};XrXirValue name={0};
        CHECK(xr_xir_vm_debug_snapshot_frame(escaped,0,&frame,&name)==XR_XIR_VALUE_OK);
        const char *bytes=NULL;size_t length=0;CHECK(xr_xir_string_view(&name,&bytes,&length) && length);
        xr_xir_value_drop(&name);xr_xir_vm_debug_snapshot_drop(escaped);
    }
    handler_close(&fixture);
}
static const HandlerCase handler_cases[]={
    {"enum E { A, B }\nenum F { No }\nfn fail()->i64 {throw E.B}\nfn run()->i64 {\n"
     "try {return fail()}\ncatch(E.A) {return 1}\ncatch(e:F) {return 2}\ncatch(E.B) {return 37} // ACCEPT selected\n}\nprint(run())\n",
     "37\n",{"ACCEPT selected"},3,1},
    {"enum E { Bad {code:i64} }\nfn run()->i64 {\ntry {throw E.Bad{code:9}}\n"
     "catch(E.Bad{code:1..=5}) {return 1}\ncatch(E.Bad{code:x}) {return x} // ACCEPT payload\n}\nprint(run())\n",
     "9\n",{"ACCEPT payload"},2,1},
    {"enum E { Bad }\nfn run()->i64 {\ntry {\ntry {throw E.Bad}\ncatch(e:E) {throw e} // ACCEPT inner\n"
     "} catch(e:E) {return 53} // ACCEPT outer\n}\nprint(run())\n",
     "53\n",{"ACCEPT inner","ACCEPT outer"},2,2},
    {"enum E { Bad }\nfn run()->i64 {\ntry {throw E.Bad}\ncatch(e:E) { // ACCEPT empty\n}\nreturn 17\n}\nprint(run())\n",
     "17\n",{"ACCEPT empty"},1,1},
    {"enum E { Bad }\nfn run()->i64 {\ntry {throw E.Bad}\ncatch(E.Bad) { // ACCEPT empty-pattern\n}\nreturn 19\n}\nprint(run())\n",
     "19\n",{"ACCEPT empty-pattern"},1,1},
    {"enum E { Bad }\nfn run()->i64 {try {return 1/0} catch(e:E) {return 5} catch panic {return 17}}\nprint(run())\n",
     "17\n",{NULL},0,0},
    {"enum E { Bad }\nfn run()->i64 {try {} catch(e:E) {return 5};return 61}\nprint(run())\n",
     "61\n",{NULL},0,0},
    {"enum E { Bad }\nfn run<T>(value:T)->T {\ntry {throw E.Bad}\ncatch(e:E) {return value} // ACCEPT generic\n}\n"
     "print(run<i64>(13))\nprint(run<string>(\"x\"))\n",
     "13\nx\n",{"ACCEPT generic","ACCEPT generic"},2,2}
};
static size_t handler_fault(size_t fail) {
    HandlerFixture fixture={0};handler_open(&fixture);size_t begin=runtime_attempts;
    runtime_fail_at=fail==SIZE_MAX ? SIZE_MAX : begin+fail;
    XrXirStatus status=handler_build(&fixture,handler_cases[0].text);
    size_t sites=runtime_attempts-begin;runtime_fail_at=SIZE_MAX;
    if (fail==SIZE_MAX) CHECK(status==XR_XIR_OK && fixture.launch);
    else {
        CHECK(status==XR_XIR_OUT_OF_MEMORY && !fixture.launch && sites>fail);
        CHECK(fixture.diagnostic.status==XR_SOURCE_COMPILE_OUT_OF_MEMORY);
        XrCompileResourceStats spent={0},retry={0};CHECK(xr_compile_resources_stats(fixture.resources,&spent)==XR_COMPILE_RESOURCE_OK);
        bool sticky=xr_compile_state_status(xr_source_compiler_state(fixture.compiler))==XR_COMPILE_RESOURCE_OUT_OF_MEMORY;
        xr_source_compile_diagnostic_free(&fixture.diagnostic);status=handler_build(&fixture,handler_cases[0].text);
        CHECK(status==(sticky ? XR_XIR_OUT_OF_MEMORY : XR_XIR_OK));
        CHECK(sticky ? !fixture.launch : fixture.launch!=NULL);
        CHECK(xr_compile_resources_stats(fixture.resources,&retry)==XR_COMPILE_RESOURCE_OK);
        CHECK(retry.work>=spent.work && retry.allocated_bytes>=spent.allocated_bytes);
    }
    handler_close(&fixture);return sites;
}
int main(int argc,char **argv) {
    xr_test_suppress_dialogs();bool resources=argc==2 && !strcmp(argv[1],"--resources");CHECK(argc==1 || resources);
    for (uint32_t i=0;i<sizeof(handler_cases)/sizeof(*handler_cases);++i) handler_case(&handler_cases[i]);
    size_t sites=handler_fault(SIZE_MAX);CHECK(sites);
    if (resources) for (size_t point=0;point<sites;++point) handler_fault(point);
    else { handler_fault(0);if (sites>1) handler_fault(sites-1); }
    printf("Source catch sites: typed order, patterns, rethrow, empty, panic/dead exclusion, specialization, producer death; launch sites=%zu injection=%s physical=0/0 PASS\n",
        sites,resources ? "ALL" : "FIRST_LAST_ONLY");return 0;
}
