/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_exception_policy.c - Real first-chance filter decisions on each execution flow
 */
#define XR_DAP_ROUTE_ENTRY reference_exception_route_main
#include "test_source_routes.c"
#include "xir/xxir_instance_debug_internal.h"
#include "xir/xxir_internal.h"
#include "xir/xxir_checked.h"
#include "exception_policy_controls.h"
static bool policy_selected(uint32_t filters,bool caught) {
    return (filters&XR_XIR_VM_DEBUG_EXCEPTION_ALL) ||
        (filters&(caught ? XR_XIR_VM_DEBUG_EXCEPTION_CAUGHT : XR_XIR_VM_DEBUG_EXCEPTION_UNCAUGHT));
}
static void policy_case(const RouteCase *expected,uint32_t filters) {
    HandlerFixture fixture={0};handler_open(&fixture);XrXirStatus built=handler_build(&fixture,expected->text);
    if (built!=XR_XIR_OK) fprintf(stderr,"policy source=%u %s\n",built,fixture.diagnostic.source.source.message);
    CHECK(built==XR_XIR_OK);RoutePoint points[32];uint32_t count=route_points(&fixture,points,32);
    XrXirValueStatus configured=xdap_current_break_on_throw(fixture.launch->control,filters);
    if (configured!=XR_XIR_VALUE_OK) fprintf(stderr,"policy flags=%u configure=%u\n",filters,configured);
    CHECK(configured==XR_XIR_VALUE_OK);
    CHECK(xdap_current_break_on_throw(fixture.launch->control,8)==XR_XIR_VALUE_BAD_ARGUMENT);
    xr_xir_compile_source_product_free(fixture.launch->source);fixture.launch->source=NULL;
    xr_source_compiler_free(fixture.compiler);fixture.compiler=NULL;
    CHECK(xdap_current_start(fixture.launch->control,fixture.launch->entry,NULL,0)==XR_XIR_CALL_READY);
    uint32_t next=0,stops=0,wanted=0;XrXirVmDebugSnapshot *escaped=NULL;uint32_t saved=0;
    for (uint32_t i=0;i<expected->stops;++i) wanted+=policy_selected(filters,expected->accepted[i]!=NULL);
    for (uint32_t polls=0;polls<64;++polls) {
        XdapCurrentResult result={0};XrXirValueStatus status=xdap_current_poll(fixture.launch->control,1000,&result);
        CHECK(status==XR_XIR_VALUE_OK);
        if (result.state==XDAP_CURRENT_STOPPED) {
            while (next<expected->stops && !policy_selected(filters,expected->accepted[next]!=NULL)) ++next;
            CHECK(next<expected->stops && fixture.length==strlen(expected->prefix[next]));
            CHECK(!memcmp(fixture.output,expected->prefix[next],fixture.length));
            XrXirVmDebugSnapshot *snapshot=NULL;CHECK(xdap_current_snapshot(fixture.launch->control,&snapshot)==XR_XIR_VALUE_OK);
            route_snapshot(snapshot,expected,next,points,count);uint32_t actual=0;
            CHECK(xir_vm_debug_snapshot_exception_filter(snapshot,&actual)==XR_XIR_VALUE_OK);
            uint32_t mode=filters&XR_XIR_VM_DEBUG_EXCEPTION_ALL ? XR_XIR_VM_DEBUG_EXCEPTION_ALL :
                expected->accepted[next] ? XR_XIR_VM_DEBUG_EXCEPTION_CAUGHT : XR_XIR_VM_DEBUG_EXCEPTION_UNCAUGHT;
            CHECK(actual==mode);
            if (!escaped) { escaped=snapshot;saved=actual; } else xr_xir_vm_debug_snapshot_drop(snapshot);
            CHECK(xdap_current_break_on_throw(fixture.launch->control,filters)==XR_XIR_VALUE_OK);
            ++next;++stops;CHECK(xdap_current_continue(fixture.launch->control,result.debug_epoch,XR_XIR_VM_DEBUG_CONTINUE)==XR_XIR_VALUE_OK);
        } else if (result.state==XDAP_CURRENT_ENDED) {
            CHECK(result.outcome.status==expected->terminal);xdap_current_result_drop(&result);break;
        }
        xdap_current_result_drop(&result);CHECK(polls<63);
    }
    CHECK(stops==wanted && fixture.length==strlen(expected->output) && !memcmp(fixture.output,expected->output,fixture.length));
    CHECK(xdap_launch_free(fixture.launch)==XR_XIR_CALL_READY);fixture.launch=NULL;
    if (escaped) { uint32_t after=0;CHECK(xir_vm_debug_snapshot_exception_filter(escaped,&after)==XR_XIR_VALUE_OK && after==saved); }
    xr_xir_vm_debug_snapshot_drop(escaped);handler_close(&fixture);
}
static const RouteCase policy_rethrow={
    "enum E{A,B}\nfn run()->i64{try{throw E.A}catch(E.A){print(\"inner\");throw E.B} // ACCEPT first\n}\nprint(run())\n",
    "inner\n",{"ACCEPT first",NULL},{"","inner\n"},{0,0},2,XR_XIR_CALL_THROWN,false
};
static const RouteCase policy_task_caught={
    "enum E{Bad}\nfn work()->i64{try{throw E.Bad}catch(E.Bad){return 127} // ACCEPT child\n}\n"
    "const job=go work()\nprint(await job)\n",
    "127\n",{"ACCEPT child"},{""},{0},1,XR_XIR_CALL_RETURNED,true
};
static void policy_rejections(void) {
    XirInstanceDebugActivation output={true,123,456},saved=output;XrXirCallView retired={0};
    CHECK(xir_instance_debug_activation(NULL,&output)==XR_XIR_CALL_BAD_STATE && output.child==saved.child && output.driver==saved.driver && output.epoch==saved.epoch);
    CHECK(xir_instance_debug_activation(&retired,&output)==XR_XIR_CALL_BAD_STATE && output.child==saved.child && output.driver==saved.driver && output.epoch==saved.epoch);
    uint32_t filter=123;CHECK(xir_vm_debug_snapshot_exception_filter(NULL,&filter)==XR_XIR_VALUE_BAD_ARGUMENT && filter==123);
}
int main(int argc,char **argv) {
    xr_test_suppress_dialogs();bool resources=argc==2 && !strcmp(argv[1],"--resources");CHECK(argc==1 || resources);
    policy_rejections();policy_omitted_route();policy_independent_breakpoint();
    static const uint32_t filters[]={0,1,2,4,6,7};
    for (uint32_t c=0;c<sizeof(route_cases)/sizeof(*route_cases);++c) for (uint32_t f=0;f<sizeof(filters)/sizeof(*filters);++f) {
        fprintf(stderr,"policy case%u flags%u\n",c,filters[f]);policy_case(&route_cases[c],filters[f]);
    }
    for (uint32_t f=0;f<sizeof(filters)/sizeof(*filters);++f) {
        policy_case(&policy_rethrow,filters[f]);policy_case(&policy_task_caught,filters[f]);
    }
    for (uint32_t mode=0;mode<3;++mode) {
        size_t sites=policy_capture_fault(mode,SIZE_MAX);CHECK(sites);
        if (resources) for (size_t fail=0;fail<sites;++fail) policy_capture_fault(mode,fail);
        else { policy_capture_fault(mode,0);policy_capture_fault(mode,sites-1); }
        printf("Filter capture mode%u sites=%zu injection=%s physical0/0 PASS\n",mode,sites,resources ? "ALL" : "FIRST_LAST_ONLY");
    }
    puts("Observed VM filters:12 ancestor cases plus rethrow/local Task acceptance x6 policies; first-chance skip, original outcomes, flow identity and owned modes physical0/0 PASS");return 0;
}
