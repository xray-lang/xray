/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_routes.c - Actual VM ancestors select handlers before unwinding
 */
#define main reference_route_launch_main
#include "test_launch.c"
#undef main
#include "handler_fixture.h"
#include "xir/xxir_vm_debug_internal.h"
typedef struct RouteCase {
    const char *text,*output,*accepted[2],*prefix[2];
    uint32_t ancestors[2],stops;
    XrXirCallStatus terminal;
    bool child;
} RouteCase;
static const RouteCase route_cases[]={
    {"enum E{Bad{code:i64}}\nfn fail()->i64{throw E.Bad{code:9}}\n"
     "fn run()->i64{try{return fail()}catch(E.Bad{code:1..=5}){return 1}\n"
     "catch(E.Bad{code:6..=10}){return 71} // ACCEPT direct\n}\nprint(run())\n",
     "71\n",{"ACCEPT direct"},{""},{1},1,XR_XIR_CALL_RETURNED,false},
    {"enum E{Bad{text:string}}\nfn fail()->i64{throw E.Bad{text:\"x\\0中\"}}\n"
     "fn relay()->i64{return fail()}\nfn run()->i64{try{return relay()}\n"
     "catch(E.Bad{text:\"x\\0中\"}){return 73} // ACCEPT relay\n}\nprint(run())\n",
     "73\n",{"ACCEPT relay"},{""},{2},1,XR_XIR_CALL_RETURNED,false},
    {"enum E{A,B}\nfn fail()->i64{throw E.B}\nfn relay()->i64{try{return fail()}catch(E.A){return 1}}\n"
     "fn run()->i64{try{return relay()}catch(E.B){return 79} // ACCEPT outer\n}\nprint(run())\n",
     "79\n",{"ACCEPT outer"},{""},{2},1,XR_XIR_CALL_RETURNED,false},
    {"enum E{A,B}\nfn rec(n:i64)->i64{try{if(n==0){throw E.B};return rec(n-1)}catch(E.A){return 1}}\n"
     "fn run()->i64{try{return rec(3)}catch(E.B){return 83} // ACCEPT recursion\n}\nprint(run())\n",
     "83\n",{"ACCEPT recursion"},{""},{4},1,XR_XIR_CALL_RETURNED,false},
    {"enum E{Bad}\nfn fail()->i64{throw E.Bad}\nfn run(f:fn()->i64)->i64{try{return f()}\n"
     "catch(E.Bad){return 87} // ACCEPT indirect\n}\nprint(run(fail))\n",
     "87\n",{"ACCEPT indirect"},{""},{1},1,XR_XIR_CALL_RETURNED,false},
    {"enum E{Bad}\nfn fail()->i64{throw E.Bad}\nfn relay()->i64{try{return fail()}\n"
     "catch(e:E){print(\"inner\");throw e} // ACCEPT inner\n}\n"
     "fn run()->i64{try{return relay()}catch(E.Bad){return 89} // ACCEPT rethrow\n}\nprint(run())\n",
     "inner\n89\n",{"ACCEPT inner","ACCEPT rethrow"},{"","inner\n"},{1,1},2,XR_XIR_CALL_RETURNED,false},
    {"enum E{Bad}\nfn fail()->i64{throw E.Bad}\nfn relay()->i64{return fail()}\nprint(relay())\n",
     "",{NULL},{""},{0},1,XR_XIR_CALL_THROWN,false},
    {"enum E{Bad}\nfn fail()->i64{throw E.Bad}\nconst job=go fail()\ntry{print(await job)}"
     "catch(E.Bad){print(97)} // ACCEPT task boundary\n",
     "97\n",{NULL},{""},{0},1,XR_XIR_CALL_RETURNED,true},
    {"enum E{Bad}\nfn rec(n:i64)->i64{if(n==0){throw E.Bad};try{return rec(n-1)}\n"
     "catch(E.Bad){return 101} // ACCEPT recursive frame\n}\nprint(rec(3))\n",
     "101\n",{"ACCEPT recursive frame"},{""},{1},1,XR_XIR_CALL_RETURNED,false},
    {"enum E<T>{Bad{value:T}}\nfn leaf()->i64{print(\"leaf\");return 103}\n"
     "fn fail<T>(v:T)->T{throw E<T>.Bad{value:v}}\nfn run<T>(v:T)->T{try{return fail(v)}\n"
     "catch(E<T>.Bad{value}){return value} // ACCEPT callable\n}\nconst f=run(leaf)\nprint(f())\n",
     "leaf\n103\n",{"ACCEPT callable"},{""},{1},1,XR_XIR_CALL_RETURNED,false},
    {"enum E<T>{Bad{value:T}}\nfn fail<T>(v:T)->T{throw E<T>.Bad{value:v}}\n"
     "fn run<T>(v:T)->T{try{return fail(v)}catch(E<T>.Bad{value}){return value} // ACCEPT generic\n}\n"
     "print(run<i64>(107))\nprint(run<string>(\"x\"))\n",
     "107\nx\n",{"ACCEPT generic","ACCEPT generic"},{"","107\n"},{1,1},2,XR_XIR_CALL_RETURNED,false},
    {"enum E{Bad}\nfn fail()->i64{defer{print(\"leaf cleanup\")};throw E.Bad}\n"
     "fn relay()->i64{defer{print(\"relay cleanup\")};return fail()}\n"
     "fn run()->i64{try{return relay()}catch(E.Bad){return 113} // ACCEPT cleanup\n}\nprint(run())\n",
     "leaf cleanup\nrelay cleanup\n113\n",{"ACCEPT cleanup"},{""},{2},1,XR_XIR_CALL_RETURNED,false}
};
typedef struct RoutePoint { uint32_t function,instruction,line; } RoutePoint;
static uint32_t route_points(const HandlerFixture *fixture,RoutePoint *points,uint32_t capacity) {
    const XrXirSourceDebugView *source=xr_xir_compile_source_product_debug(fixture->launch->source);CHECK(source);
    uint32_t count=0;
    for (uint32_t f=0;f<source->function_count;++f) {
        const XrXirSourceDebugFunction *function=&source->functions[f];
        for (uint32_t h=0;h<function->handler_count;++h) for (uint32_t s=0;s<function->handlers[h].selector_count;++s) {
            uint32_t instruction=function->handlers[h].selectors[s].accepted;CHECK(count<capacity);
            points[count++]=(RoutePoint){f,instruction,(uint32_t)function->points[instruction].range.line};
        }
    }
    return count;
}
static XirVmDebugExceptionRoute route_snapshot(XrXirVmDebugSnapshot *snapshot,const RouteCase *expected,
    uint32_t stop,const RoutePoint *points,uint32_t count) {
    XirVmDebugExceptionRoute route={0};XirVmDebugExceptionLocal local={0};
    CHECK(xir_vm_debug_snapshot_exception_route(snapshot,&route)==XR_XIR_VALUE_OK);
    CHECK(xir_vm_debug_snapshot_exception_local(snapshot,&local)==XR_XIR_VALUE_OK);
    XrXirVmDebugFrameInfo current={0};XrXirValue name={0};
    CHECK(xr_xir_vm_debug_snapshot_frame(snapshot,0,&current,&name)==XR_XIR_VALUE_OK);xr_xir_value_drop(&name);
    CHECK(local.function==current.function && local.kind==XIR_VM_DEBUG_EXCEPTION_LOCAL_PROPAGATE);
    CHECK(expected->child ? current.thread>1 : current.thread==1);
    if (!expected->accepted[stop]) {
        CHECK(route.kind==XIR_VM_DEBUG_EXCEPTION_ROUTE_UNKNOWN && !route.frame && !route.handler &&
            route.function==UINT32_MAX && route.accepted==UINT32_MAX);return route;
    }
    CHECK(route.kind==XIR_VM_DEBUG_EXCEPTION_ROUTE_SELECTED && route.handler && route.frame &&
        route.ancestors==expected->ancestors[stop] && route.frame!=current.id);
    bool found=false;
    for (uint32_t i=0;i<count;++i) if (points[i].function==route.function && points[i].instruction==route.accepted)
        found=points[i].line==handler_line(expected->text,expected->accepted[stop]);
    CHECK(found);
    XrXirVmDebugFrameInfo selected={0};
    CHECK(xr_xir_vm_debug_snapshot_frame(snapshot,route.ancestors,&selected,&name)==XR_XIR_VALUE_OK);xr_xir_value_drop(&name);
    CHECK(selected.id==route.frame && selected.function==route.function && selected.thread==current.thread && selected.code==XR_XIR_VM_DEBUG_VM);
    for (uint32_t i=0;i<route.ancestors;++i) {
        XrXirVmDebugFrameInfo middle={0};CHECK(xr_xir_vm_debug_snapshot_frame(snapshot,i,&middle,&name)==XR_XIR_VALUE_OK);
        xr_xir_value_drop(&name);CHECK(middle.id!=route.frame);
    }
    return route;
}
static void route_case(const RouteCase *expected,bool observe) {
    HandlerFixture fixture={0};handler_open(&fixture);XrXirStatus status=handler_build(&fixture,expected->text);
    if (status!=XR_XIR_OK) fprintf(stderr,"route source=%u %s\n",status,fixture.diagnostic.source.source.message);
    CHECK(status==XR_XIR_OK);RoutePoint points[32];uint32_t count=route_points(&fixture,points,32);
    CHECK(xdap_current_break_on_throw(fixture.launch->control,observe)==XR_XIR_VALUE_OK);
    xr_xir_compile_source_product_free(fixture.launch->source);fixture.launch->source=NULL;
    xr_source_compiler_free(fixture.compiler);fixture.compiler=NULL;
    CHECK(xdap_current_start(fixture.launch->control,fixture.launch->entry,NULL,0)==XR_XIR_CALL_READY);
    XrXirVmDebugSnapshot *escaped=NULL;XirVmDebugExceptionRoute saved={0};uint32_t stops=0;
    for (uint32_t polls=0;polls<64;++polls) {
        XdapCurrentResult result={0};XrXirValueStatus polled=xdap_current_poll(fixture.launch->control,1000,&result);
        if (polled!=XR_XIR_VALUE_OK) fprintf(stderr,"route poll=%u state=%u stop%u\n",polled,result.state,stops);
        CHECK(polled==XR_XIR_VALUE_OK);
        if (result.state==XDAP_CURRENT_STOPPED) {
            CHECK(stops<expected->stops && fixture.length==strlen(expected->prefix[stops]));
            CHECK(!memcmp(fixture.output,expected->prefix[stops],fixture.length));
            XrXirVmDebugSnapshot *snapshot=NULL;CHECK(xdap_current_snapshot(fixture.launch->control,&snapshot)==XR_XIR_VALUE_OK);
            XirVmDebugExceptionRoute route=route_snapshot(snapshot,expected,stops,points,count);
            if (!escaped) { escaped=snapshot;saved=route; } else xr_xir_vm_debug_snapshot_drop(snapshot);
            ++stops;CHECK(xdap_current_continue(fixture.launch->control,result.debug_epoch,XR_XIR_VM_DEBUG_CONTINUE)==XR_XIR_VALUE_OK);
        } else if (result.state==XDAP_CURRENT_ENDED) {
            if (result.outcome.status!=expected->terminal) fprintf(stderr,"route outcome=%u wanted%u\n",result.outcome.status,expected->terminal);
            CHECK(result.outcome.status==expected->terminal);xdap_current_result_drop(&result);break;
        }
        xdap_current_result_drop(&result);CHECK(polls<63);
    }
    CHECK(stops==(observe ? expected->stops : 0) && fixture.length==strlen(expected->output) && !memcmp(fixture.output,expected->output,fixture.length));
    CHECK(xdap_launch_free(fixture.launch)==XR_XIR_CALL_READY);fixture.launch=NULL;
    if (escaped) {
        XirVmDebugExceptionRoute after={0};CHECK(xir_vm_debug_snapshot_exception_route(escaped,&after)==XR_XIR_VALUE_OK);
        CHECK(after.kind==saved.kind && after.function==saved.function && after.handler==saved.handler &&
            after.accepted==saved.accepted && after.frame==saved.frame && after.ancestors==saved.ancestors);
        CHECK(xir_vm_debug_snapshot_exception_route(NULL,&after)==XR_XIR_VALUE_BAD_ARGUMENT && after.frame==saved.frame);
    }
    xr_xir_vm_debug_snapshot_drop(escaped);handler_close(&fixture);
}
static size_t route_capture_fault(size_t fail) {
    HandlerFixture fixture={0};handler_open(&fixture);CHECK(handler_build(&fixture,route_cases[2].text)==XR_XIR_OK);
    CHECK(xdap_current_break_on_throw(fixture.launch->control,true)==XR_XIR_VALUE_OK);
    xr_xir_compile_source_product_free(fixture.launch->source);fixture.launch->source=NULL;
    xr_source_compiler_free(fixture.compiler);fixture.compiler=NULL;
    size_t begin=runtime_attempts;runtime_fail_at=fail==SIZE_MAX ? SIZE_MAX : begin+fail;
    XrXirCallStatus start=xdap_current_start(fixture.launch->control,fixture.launch->entry,NULL,0);XdapCurrentResult result={0};
    if (start==XR_XIR_CALL_READY) {
        CHECK(xdap_current_poll(fixture.launch->control,1000,&result)==XR_XIR_VALUE_OK);
        CHECK(fail==SIZE_MAX ? result.state==XDAP_CURRENT_STOPPED : result.state==XDAP_CURRENT_ENDED && result.outcome.status==XR_XIR_CALL_OOM);
    } else CHECK(fail!=SIZE_MAX && start==XR_XIR_CALL_OOM);
    size_t sites=runtime_attempts-begin;runtime_fail_at=SIZE_MAX;CHECK(fail==SIZE_MAX || sites>fail);
    CHECK(!fixture.length);xdap_current_result_drop(&result);
    XrXirCallStatus freed=xdap_launch_free(fixture.launch);fixture.launch=NULL;
    CHECK(freed==XR_XIR_CALL_READY || (fail!=SIZE_MAX && freed==XR_XIR_CALL_OOM));handler_close(&fixture);return sites;
}
typedef struct RouteBudgetProbe { XrXirDomainBudgetStats stats;bool stopped; } RouteBudgetProbe;
static RouteBudgetProbe route_budget_probe(uint64_t work,uint64_t metadata) {
    HandlerFixture fixture={0};handler_open(&fixture);xr_xir_domain_drop(fixture.domain);fixture.domain=NULL;
    XrXirDomainBudgetControls budget={1048576,1048576,work,metadata,1048576};
    CHECK(xr_xir_domain_new_budgeted(1048576,&budget,&fixture.domain)==XR_XIR_VALUE_OK);
    XrXirStatus built=handler_build(&fixture,route_cases[2].text);
    bool denied=built==XR_XIR_BUDGET;
    if (!denied) {
        CHECK(built==XR_XIR_OK);
        XrXirValueStatus policy=xdap_current_break_on_throw(fixture.launch->control,true);
        denied=policy==XR_XIR_VALUE_LIMIT;CHECK(denied || policy==XR_XIR_VALUE_OK);
        if (!denied) {
            CHECK(xdap_current_start(fixture.launch->control,fixture.launch->entry,NULL,0)==XR_XIR_CALL_READY);
            XdapCurrentResult result={0};XrXirValueStatus polled=xdap_current_poll(fixture.launch->control,1000,&result);
            denied=polled==XR_XIR_VALUE_LIMIT || (result.state==XDAP_CURRENT_ENDED && result.outcome.status==XR_XIR_CALL_LIMIT);
            CHECK(denied || (polled==XR_XIR_VALUE_OK && result.state==XDAP_CURRENT_STOPPED && result.debug_epoch));
            xdap_current_result_drop(&result);
        }
    }
    CHECK(!fixture.length);RouteBudgetProbe result={xr_xir_domain_budget_stats(fixture.domain),!denied};
    XrXirCallStatus freed=xdap_launch_free(fixture.launch);fixture.launch=NULL;
    CHECK(freed==XR_XIR_CALL_READY || (denied && freed==XR_XIR_CALL_LIMIT));handler_close(&fixture);return result;
}
static RouteBudgetProbe route_budget_case(uint64_t work,uint64_t metadata,bool success) {
    RouteBudgetProbe result=route_budget_probe(work,metadata);
    if (result.stopped!=success) fprintf(stderr,"route budget work=%llu metadata=%llu success=%u stopped=%u used=%llu peak=%llu\n",
        (unsigned long long)work,(unsigned long long)metadata,(unsigned)success,(unsigned)result.stopped,
        (unsigned long long)result.stats.work,(unsigned long long)result.stats.metadata_peak);
    CHECK(result.stopped==success);return result;
}
static void route_budgets(void) {
    RouteBudgetProbe baseline=route_budget_case(128000000,1048576,true);
    CHECK(baseline.stats.work && baseline.stats.metadata_peak);
    /* A completed stop may precede deferred observer maintenance. Discover the
     * actual publication threshold instead of treating all later work as needed. */
    uint64_t low=1,high=baseline.stats.work;CHECK(!route_budget_probe(low,1048576).stopped);
    while (high-low>1) {
        uint64_t middle=low+(high-low)/2;
        if (route_budget_probe(middle,1048576).stopped) high=middle;else low=middle;
    }
    route_budget_case(high,1048576,true);route_budget_case(high-1,1048576,false);
    route_budget_case(128000000,baseline.stats.metadata_peak,true);route_budget_case(128000000,baseline.stats.metadata_peak-1,false);
    printf("Route observer exact/minus-one work=%llu metadata=%llu no handler output/physical0/0 PASS\n",
        (unsigned long long)high,(unsigned long long)baseline.stats.metadata_peak);
}
#include "route_write_fixture.h"
int main(int argc,char **argv) {
    xr_test_suppress_dialogs();
    if (argc==3 && (!strcmp(argv[1],"--case") || !strcmp(argv[1],"--plain-case"))) {
        char *end=NULL;unsigned long which=strtoul(argv[2],&end,10);
        CHECK(end && end!=argv[2] && !*end && which<sizeof(route_cases)/sizeof(*route_cases));
        route_case(&route_cases[which],!strcmp(argv[1],"--case"));return 0;
    }
    bool resources=argc==2 && !strcmp(argv[1],"--resources");CHECK(argc==1 || resources);
    for (uint32_t i=0;i<sizeof(route_cases)/sizeof(*route_cases);++i) {
        fprintf(stderr,"route case%u observe=1\n",i);route_case(&route_cases[i],true);
    }
    route_unauthenticated();route_budgets();size_t sites=route_capture_fault(SIZE_MAX),writes=route_write_fault(SIZE_MAX);CHECK(sites && writes);
    if (resources) for (size_t fail=0;fail<sites;++fail) route_capture_fault(fail);
    else { route_capture_fault(0);route_capture_fault(sites-1); }
    if (resources) for (size_t fail=0;fail<writes;++fail) route_write_fault(fail);
    else { route_write_fault(0);route_write_fault(writes-1); }
    printf("Route stop write retains selection and old/new Cell5/7 through producer/Program death; write sites=%zu injection=%s physical0/0 PASS\n",
        writes,resources ? "ALL" : "FIRST_LAST_ONLY");
    printf("VM ancestor routes: direct/relay/rejected/recursive/indirect/rethrow, root/Task unknown; capture sites=%zu injection=%s physical0/0 PASS\n",
        sites,resources ? "ALL" : "FIRST_LAST_ONLY");return 0;
}
