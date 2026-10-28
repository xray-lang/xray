/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_throw_sites.c - Genuine throws survive local routing and owner death
 */
#define main reference_source_throw_launch_main
#include "test_launch.c"
#undef main
#include "handler_fixture.h"
#include "xir/xxir_source_query_internal.h"

typedef struct ThrowSiteCase {
    const char *text,*output,*markers[3],*prefixes[3];
    uint32_t sites,stops;
} ThrowSiteCase;
static const ThrowSiteCase throw_site_cases[]={
    {"enum E{Bad}\nfn run()->i64{\ntry{throw E.Bad} // THROW local\ncatch(e:E){return 7}\n}\nprint(run())\n",
     "7\n",{"THROW local"},{""},1,1},
    {"enum E{Bad}\nfn run()->i64{try{\ntry{throw E.Bad} // THROW inner\n"
     "catch(e:E){print(\"inner\");\nthrow e} // THROW again\n}catch(e:E){return 11}}\nprint(run())\n",
     "inner\n11\n",{"THROW inner","THROW again"},{"","inner\n"},2,2},
    {"enum E{A,B}\nfn fail()->i64{\ntry{throw E.B} // THROW unmatched\ncatch(E.A){return 1}}\n"
     "fn run()->i64{try{return fail()}catch(e:E){return 19}}\nprint(run())\n",
     "19\n",{"THROW unmatched"},{""},1,1},
    {"enum E{Bad}\nfn fail(error:Error)->i64{\nthrow error // THROW erased\n}\n"
     "fn run()->i64{try{return fail(E.Bad)}catch(e:E){return 23}}\nprint(run())\n",
     "23\n",{"THROW erased"},{""},1,1},
    {"enum E{Bad}\nfn run<T>(value:T)->T{\ntry{throw E.Bad} // THROW generic\ncatch(e:E){return value}}\n"
     "print(run<i64>(13))\nprint(run<string>(\"x\"))\n",
     "13\nx\n",{"THROW generic","THROW generic"},{"","13\n"},2,2},
    {"enum E{Bad}\nfn run()->i64{try{defer{print(\"cleanup\")}\n"
     "throw E.Bad // THROW cleanup\n}catch(e:E){print(\"caught\");return 31}}\nprint(run())\n",
     "cleanup\ncaught\n31\n",{"THROW cleanup"},{""},1,1},
    {"enum E{Bad}\nfn run()->i64{try{return 37}catch(e:E){throw e}}\nprint(run())\n",
     "37\n",{NULL},{NULL},0,0},
    {"fn run()->i64{try{return 1/0}catch panic{return 41}}\nprint(run())\n",
     "41\n",{NULL},{NULL},0,0}
};
static XrXirSourceSnapshot *throw_site_publication(HandlerFixture *fixture,const XrXirSourceDebugView *debug,uint32_t f) {
    const XrXirCompileContext *context=xr_source_compiler_context(fixture->compiler);
    XrXirSourceProductPacketView packet={0};XrXirArtifact *checked=NULL;
    CHECK(xr_xir_compile_source_product_packet(fixture->launch->source,XR_XIR_SOURCE_PRODUCT_CLOSED,&packet)==XR_XIR_OK);
    CHECK(xr_xir_compile_checked_read(context,packet.bytes,packet.length,&checked,NULL)==XR_XIR_OK);
    const XrXirModule *module=xr_xir_compile_artifact_module(checked);
    XrXirSourceSnapshot *snapshot=NULL;
    CHECK(xr_xir_compile_source_snapshot_copy_v2(context,xr_xir_compile_source_product_view(fixture->launch->source),
        xr_xir_compile_source_product_construction(fixture->launch->source),&snapshot)==XR_XIR_OK);
    size_t bytes=(size_t)debug->function_count*sizeof(*debug->functions);
    XrXirSourceDebugFunction *functions=NULL;XrXirSourceDebugThrow *sites=NULL;
    CHECK(xr_compile_resources_alloc(fixture->resources,bytes,(void **)&functions)==XR_COMPILE_RESOURCE_OK);
    memcpy(functions,debug->functions,bytes);uint32_t count=functions[f].throw_count;CHECK(count);
    size_t site_bytes=((size_t)count+1)*sizeof(*sites);
    CHECK(xr_compile_resources_alloc(fixture->resources,site_bytes,(void **)&sites)==XR_COMPILE_RESOURCE_OK);
    memcpy(sites,functions[f].throw_sites,(size_t)count*sizeof(*sites));functions[f].throw_sites=sites;
    CHECK(xr_compile_resources_work(fixture->resources,bytes+site_bytes)==XR_COMPILE_RESOURCE_OK);
    XrXirSourceDebugView candidate=*debug;candidate.functions=functions;
    XrXirSourceDebugFunction saved=functions[f];XrXirSourceDebugThrow first=sites[0];
    XrCompileResourceStats before={0},after={0};CHECK(xr_compile_resources_stats(fixture->resources,&before)==XR_COMPILE_RESOURCE_OK);
    for (uint32_t bad=0;bad<7;++bad) {
        functions[f]=saved;sites[0]=first;
        if (bad==0) sites[0].instruction=functions[f].instruction_count;
        else if (bad==1) sites[0].operand=UINT32_MAX;
        else if (bad==2) sites[0].operand=functions[f].parameter_count+first.instruction;
        else if (bad==3) { sites[count]=sites[count-1];functions[f].throw_count=count+1; }
        else if (bad==4) functions[f].points=NULL;
        else if (bad==5) functions[f].throw_count=0;
        else functions[f].throw_sites=NULL;
        CHECK(xr_xir_compile_source_snapshot_debug_copy(snapshot,&candidate,module)==XR_XIR_BAD_STRUCTURE);
        CHECK(!xr_xir_compile_source_snapshot_debug(snapshot));
        CHECK(xr_compile_resources_stats(fixture->resources,&after)==XR_COMPILE_RESOURCE_OK && after.live_bytes==before.live_bytes);
        CHECK(after.work>before.work);
    }
    functions[f]=saved;sites[0]=first;
    XrXirFunction *bodies=NULL;size_t body_bytes=(size_t)module->function_count*sizeof(*bodies);
    CHECK(xr_compile_resources_alloc(fixture->resources,body_bytes,(void **)&bodies)==XR_COMPILE_RESOURCE_OK);
    memcpy(bodies,module->functions,body_bytes);CHECK(xr_compile_resources_work(fixture->resources,body_bytes)==XR_COMPILE_RESOURCE_OK);
    XrXirModule malformed=*module;malformed.functions=bodies;bodies[f].instructions=NULL;
    CHECK(xr_xir_compile_source_snapshot_debug_copy(snapshot,&candidate,&malformed)==XR_XIR_BAD_STRUCTURE);
    bodies[f]=module->functions[f];malformed.types=NULL;
    CHECK(xr_xir_compile_source_snapshot_debug_copy(snapshot,&candidate,&malformed)==XR_XIR_BAD_STRUCTURE);
    CHECK(!xr_xir_compile_source_snapshot_debug(snapshot));xr_compile_resources_free(bodies);
    CHECK(xr_xir_compile_source_snapshot_debug_copy(snapshot,&candidate,module)==XR_XIR_OK);
    const XrXirSourceDebugView *owned=xr_xir_compile_source_snapshot_debug(snapshot);
    CHECK(owned && owned->functions[f].throw_sites!=sites && owned->functions[f].throw_count==count);
    memset(sites,0,site_bytes);memset(functions,0,bytes);
    CHECK(!memcmp(&owned->functions[f].throw_sites[0],&first,sizeof(first)));
    xr_compile_resources_free(sites);xr_compile_resources_free(functions);xr_xir_compile_artifact_free(checked);return snapshot;
}
static void throw_site_case(const ThrowSiteCase *expected) {
    HandlerFixture fixture={0};handler_open(&fixture);
    XrXirStatus status=handler_build(&fixture,expected->text);
    char *source_report=NULL;if(status!=XR_XIR_OK)(void)xr_frontend_report_render(fixture.diagnostic.source.failure.report,&source_report);
    if (status!=XR_XIR_OK) fprintf(stderr,"throw source=%u %s\n%s\n",status,source_report?source_report:"diagnostic unavailable",expected->text);
    xr_compile_resources_free(source_report);CHECK(status==XR_XIR_OK && fixture.launch);
    const XrXirSourceDebugView *debug=xr_xir_compile_source_product_debug(fixture.launch->source);CHECK(debug);
    XrXirVmDebugLocation locations[16];uint32_t lines[16],count=0;
    XrXirSourceSnapshot *publication=NULL;
    for (uint32_t f=0;f<debug->function_count;++f) {
        const XrXirSourceDebugFunction *function=&debug->functions[f];
        if (!publication && function->throw_count) publication=throw_site_publication(&fixture,debug,f);
        for (uint32_t t=0;t<function->throw_count;++t) {
            const XrXirSourceDebugThrow *site=&function->throw_sites[t];CHECK(count<16);
            CHECK(site->instruction<function->instruction_count && function->points[site->instruction].statement);
            CHECK(!t || function->throw_sites[t-1].instruction<site->instruction);
            locations[count]=(XrXirVmDebugLocation){f,site->instruction};lines[count++]=(uint32_t)function->points[site->instruction].range.line;
        }
    }
    CHECK(count==expected->sites);
    CHECK(xdap_current_break_on_throw(fixture.launch->control,true)==XR_XIR_VALUE_OK);
    xr_xir_compile_source_product_free(fixture.launch->source);fixture.launch->source=NULL;
    xr_source_compiler_free(fixture.compiler);fixture.compiler=NULL;
    CHECK(xdap_current_start(fixture.launch->control,fixture.launch->entry,NULL,0)==XR_XIR_CALL_READY);
    XrXirVmDebugSnapshot *escaped=NULL;XrXirValue exception={0};uint32_t stops=0;
    for (uint32_t polls=0;polls<64;++polls) {
        XdapCurrentResult result={0};CHECK(xdap_current_poll(fixture.launch->control,1000,&result)==XR_XIR_VALUE_OK);
        if (result.state==XDAP_CURRENT_STOPPED) {
            CHECK(stops<expected->stops);
            CHECK(fixture.length==strlen(expected->prefixes[stops]) && !memcmp(fixture.output,expected->prefixes[stops],fixture.length));
            XrXirVmDebugSnapshot *snapshot=NULL;CHECK(xdap_current_snapshot(fixture.launch->control,&snapshot)==XR_XIR_VALUE_OK);
            XrXirVmDebugFrameInfo frame={0};XrXirValue name={0},value={0};
            CHECK(xr_xir_vm_debug_snapshot_frame(snapshot,0,&frame,&name)==XR_XIR_VALUE_OK);xr_xir_value_drop(&name);
            bool found=false;
            for (uint32_t t=0;t<count;++t) if (locations[t].function==frame.function && locations[t].instruction==frame.instruction)
                found=lines[t]==handler_line(expected->text,expected->markers[stops]);
            CHECK(found && xr_xir_vm_debug_snapshot_exception(snapshot,&value)==XR_XIR_VALUE_OK);
            XrXirEnumBorrow borrowed={0};CHECK(xr_xir_enum_borrow(&value,&borrowed)==XR_XIR_VALUE_OK);
            CHECK(borrowed.name.length==1 && borrowed.name.bytes[0]=='E');
            if (!escaped) { escaped=snapshot;exception=value; } else { xr_xir_vm_debug_snapshot_drop(snapshot);xr_xir_value_drop(&value); }
            ++stops;CHECK(xdap_current_continue(fixture.launch->control,result.debug_epoch,XR_XIR_VM_DEBUG_CONTINUE)==XR_XIR_VALUE_OK);
        } else if (result.state==XDAP_CURRENT_ENDED) {
            CHECK(result.outcome.status==XR_XIR_CALL_RETURNED);xdap_current_result_drop(&result);break;
        }
        xdap_current_result_drop(&result);CHECK(polls<63);
    }
    CHECK(stops==expected->stops && fixture.length==strlen(expected->output) && !memcmp(fixture.output,expected->output,fixture.length));
    CHECK(xdap_launch_free(fixture.launch)==XR_XIR_CALL_READY);fixture.launch=NULL;
    if (escaped) {
        XrXirValue copy={0};CHECK(xr_xir_vm_debug_snapshot_exception(escaped,&copy)==XR_XIR_VALUE_OK);xr_xir_value_drop(&copy);
        XrXirEnumBorrow borrowed={0};CHECK(xr_xir_enum_borrow(&exception,&borrowed)==XR_XIR_VALUE_OK && borrowed.name.length==1);
    }
    if (publication) {
        const XrXirSourceDebugView *owned=xr_xir_compile_source_snapshot_debug(publication);CHECK(owned);
        CHECK(owned->functions[locations[0].function].throw_count);
    }
    xr_xir_compile_source_snapshot_free(publication);xr_xir_vm_debug_snapshot_drop(escaped);xr_xir_value_drop(&exception);
    handler_close(&fixture);
}
static size_t throw_site_capture_fault(size_t fail) {
    HandlerFixture fixture={0};handler_open(&fixture);CHECK(handler_build(&fixture,throw_site_cases[0].text)==XR_XIR_OK);
    CHECK(xdap_current_break_on_throw(fixture.launch->control,true)==XR_XIR_VALUE_OK);
    xr_xir_compile_source_product_free(fixture.launch->source);fixture.launch->source=NULL;
    xr_source_compiler_free(fixture.compiler);fixture.compiler=NULL;
    size_t begin=runtime_attempts;runtime_fail_at=fail==SIZE_MAX ? SIZE_MAX : begin+fail;
    XrXirCallStatus start=xdap_current_start(fixture.launch->control,fixture.launch->entry,NULL,0);
    XdapCurrentResult result={0};
    if (start==XR_XIR_CALL_READY) {
        CHECK(xdap_current_poll(fixture.launch->control,1000,&result)==XR_XIR_VALUE_OK);
        CHECK(fail==SIZE_MAX ? result.state==XDAP_CURRENT_STOPPED :
            result.state==XDAP_CURRENT_ENDED && result.outcome.status==XR_XIR_CALL_OOM);
    } else CHECK(fail!=SIZE_MAX && start==XR_XIR_CALL_OOM);
    size_t sites=runtime_attempts-begin;runtime_fail_at=SIZE_MAX;CHECK(fail==SIZE_MAX || sites>fail);
    CHECK(!fixture.length);xdap_current_result_drop(&result);
    XrXirCallStatus freed=xdap_launch_free(fixture.launch);fixture.launch=NULL;
    CHECK(freed==XR_XIR_CALL_READY || (fail!=SIZE_MAX && freed==XR_XIR_CALL_OOM));
    handler_close(&fixture);return sites;
}
static void throw_site_cancel_cleanup(void) {
    HandlerFixture fixture={0};handler_open(&fixture);CHECK(handler_build(&fixture,throw_site_cases[5].text)==XR_XIR_OK);
    CHECK(xdap_current_break_on_throw(fixture.launch->control,true)==XR_XIR_VALUE_OK);
    xr_xir_compile_source_product_free(fixture.launch->source);fixture.launch->source=NULL;
    xr_source_compiler_free(fixture.compiler);fixture.compiler=NULL;
    CHECK(xdap_current_start(fixture.launch->control,fixture.launch->entry,NULL,0)==XR_XIR_CALL_READY);
    XdapCurrentResult result={0};CHECK(xdap_current_poll(fixture.launch->control,1000,&result)==XR_XIR_VALUE_OK);
    CHECK(result.state==XDAP_CURRENT_STOPPED && !fixture.length);
    XrXirVmDebugSnapshot *snapshot=NULL;CHECK(xdap_current_snapshot(fixture.launch->control,&snapshot)==XR_XIR_VALUE_OK);
    uint64_t epoch=result.debug_epoch;
    CHECK(xdap_current_cancel(fixture.launch->control)==XR_XIR_CALL_CANCEL_REQUESTED);
    xdap_current_result_drop(&result);CHECK(xdap_current_poll(fixture.launch->control,1000,&result)==XR_XIR_VALUE_OK);
    CHECK(result.state==XDAP_CURRENT_ENDED && result.outcome.status==XR_XIR_CALL_CANCELLED);
    CHECK(fixture.length==8 && !memcmp(fixture.output,"cleanup\n",8));
    CHECK(xdap_current_continue(fixture.launch->control,epoch,XR_XIR_VM_DEBUG_CONTINUE)!=XR_XIR_VALUE_OK);
    xdap_current_result_drop(&result);CHECK(xdap_launch_free(fixture.launch)==XR_XIR_CALL_READY);fixture.launch=NULL;
    XrXirValue value={0};CHECK(xr_xir_vm_debug_snapshot_exception(snapshot,&value)==XR_XIR_VALUE_OK);
    xr_xir_value_drop(&value);xr_xir_vm_debug_snapshot_drop(snapshot);handler_close(&fixture);
}
int main(int argc,char **argv) {
    xr_test_suppress_dialogs();bool resources=argc==2 && !strcmp(argv[1],"--resources");CHECK(argc==1 || resources);
    for (uint32_t i=0;i<sizeof(throw_site_cases)/sizeof(*throw_site_cases);++i) throw_site_case(&throw_site_cases[i]);
    throw_site_cancel_cleanup();
    size_t sites=throw_site_capture_fault(SIZE_MAX);CHECK(sites);
    if (resources) for (size_t fail=0;fail<sites;++fail) throw_site_capture_fault(fail);
    else { throw_site_capture_fault(0);if (sites>1) throw_site_capture_fault(sites-1); }
    printf("Source throws: local routing, rethrow, unmatched propagation, erased/generic, before cleanup, dead/panic exclusion, owner death; capture sites=%zu injection=%s physical0/0 PASS\n",
        sites,resources ? "ALL" : "FIRST_LAST_ONLY");return 0;
}
