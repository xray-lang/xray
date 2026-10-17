/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_selectors.c - Actual throws consume owned closed selector facts
 */
#define main reference_selector_launch_main
#include "test_launch.c"
#undef main
#include "handler_fixture.h"
#include "xir/xxir_vm_debug_internal.h"
#include "xir/xxir_internal.h"
typedef struct SelectorCase {
    const char *text,*output,*accepted[2],*prefix[2];
    uint32_t stops;
} SelectorCase;
static const SelectorCase selector_cases[]={
    {"enum E{A,B}\nenum F{No}\nfn run()->i64{try{throw E.B}\n"
     "catch(E.A){return 1}\ncatch(F.No){return 2}\ncatch(E.B){return 31} // ACCEPT selected\n}\nprint(run())\n",
     "31\n",{"ACCEPT selected"},{""},1},
    {"enum I{X{code:i64},Y}\nenum E{Bad{inner:I,text:string,flag:bool,r:rune,f:f64}}\n"
     "fn run()->i64{try{throw E.Bad{inner:I.X{code:9},text:\"x\\0中\",flag:true,r:'中',f:1.25}}\n"
     "catch(E.Bad{inner:I.Y,text:\"x\\0中\"}){return 1}\n"
     "catch(E.Bad{inner:I.X{code:1..=5}}){return 2}\ncatch(E.Bad{text:\"wrong\"}){return 3}\n"
     "catch(E.Bad{flag:false}){return 4}\ncatch(E.Bad{r:'x'}){return 5}\ncatch(E.Bad{f:2.5}){return 6}\n"
     "catch(E.Bad{inner:I.X{code:6..=10},text:\"x\\0中\",flag:true,r:'中',f:1.25}){return 37} // ACCEPT payload\n}\nprint(run())\n",
     "37\n",{"ACCEPT payload"},{""},1},
    {"enum E{A{code:i64},B{code:i64}}\nfn run()->i64{try{throw E.B{code:11}}\n"
     "catch(E.A{code},E.B{code}){return code} // ACCEPT alternative\n}\nprint(run())\n",
     "11\n",{"ACCEPT alternative"},{""},1},
    {"enum E{A,B}\nfn run()->i64{try{\ntry{throw E.B}catch(E.A){return 1}\n"
     "}catch(E.B){return 41} // ACCEPT outer\n}\nprint(run())\n",
     "41\n",{"ACCEPT outer"},{""},1},
    {"enum E{Bad}\nfn run()->i64{try{\ntry{throw E.Bad}\ncatch(e:E){print(\"inner\"); // ACCEPT inner\nthrow e}\n"
     "}catch(e:E){return 43} // ACCEPT outer\n}\nprint(run())\n",
     "inner\n43\n",{"ACCEPT inner","ACCEPT outer"},{"","inner\n"},2},
    {"enum E<T>{Bad{value:T}}\nfn run<T>(v:T)->T{try{throw E<T>.Bad{value:v}}\n"
     "catch(E<T>.Bad{value}){return value} // ACCEPT generic\n}\nprint(run<i64>(13))\nprint(run<string>(\"x\"))\n",
     "13\nx\n",{"ACCEPT generic","ACCEPT generic"},{"","13\n"},2},
    {"enum E{Bad}\nfn run()->i64{try{defer{print(\"cleanup\")};throw E.Bad}\n"
     "catch(e){print(\"caught\");return 47} // ACCEPT all\n}\nprint(run())\n",
     "cleanup\ncaught\n47\n",{"ACCEPT all"},{""},1},
    {"enum E{A,B}\nfn fail()->i64{try{throw E.B}catch(E.A){return 1}}\n"
     "fn run()->i64{try{return fail()}catch(e:E){return 53}}\nprint(run())\n",
     "53\n",{NULL},{""},1},
    {"enum E{Bad{code:u64}}\nfn run()->i64{try{throw E.Bad{code:18446744073709551615}}\n"
     "catch(E.Bad{code:18446744073709551614..=18446744073709551615}){return 59} // ACCEPT unsigned\n}\nprint(run())\n",
     "59\n",{"ACCEPT unsigned"},{""},1},
    {"enum E{Bad{code:i64}}\nfn run(error:Error)->i64{try{throw error}\n"
     "catch(E.Bad{code:3..=7}){return 61} // ACCEPT erased\n}\nprint(run(E.Bad{code:5}))\n",
     "61\n",{"ACCEPT erased"},{""},1},
    {"enum E<T>{Bad{value:T}}\nfn leaf()->i64{print(\"leaf\");return 67}\n"
     "fn run<T>(value:T)->T{try{throw E<T>.Bad{value:value}}\ncatch(E<T>.Bad{value}){return value} // ACCEPT callable\n}\n"
     "const f=run(leaf)\nprint(f())\n",
     "leaf\n67\n",{"ACCEPT callable"},{""},1}
};
#include "selector_closed_facts.h"
static void selector_case(const SelectorCase *expected,bool generic,bool observe) {
    HandlerFixture fixture={0};handler_open(&fixture);XrXirStatus status=handler_build(&fixture,expected->text);
    if (status!=XR_XIR_OK) fprintf(stderr,"selector source=%u %s\n",status,fixture.diagnostic.source.source.message);
    CHECK(status==XR_XIR_OK);selector_closed_facts(&fixture,generic);
    const XrXirSourceDebugView *source=xr_xir_compile_source_product_debug(fixture.launch->source);CHECK(source);
    XrXirVmDebugLocation accepted[16];uint32_t accepted_lines[16],count=0;
    for (uint32_t f=0;f<source->function_count;++f) {
        const XrXirSourceDebugFunction *function=&source->functions[f];
        for (uint32_t h=0;h<function->handler_count;++h) for (uint32_t s=0;s<function->handlers[h].selector_count;++s) {
            uint32_t instruction=function->handlers[h].selectors[s].accepted;CHECK(count<16);
            accepted[count]=(XrXirVmDebugLocation){f,instruction};accepted_lines[count++]=(uint32_t)function->points[instruction].range.line;
        }
    }
    CHECK(xdap_current_break_on_throw(fixture.launch->control,observe)==XR_XIR_VALUE_OK);
    xr_xir_compile_source_product_free(fixture.launch->source);fixture.launch->source=NULL;
    xr_source_compiler_free(fixture.compiler);fixture.compiler=NULL;
    CHECK(xdap_current_start(fixture.launch->control,fixture.launch->entry,NULL,0)==XR_XIR_CALL_READY);
    XrXirVmDebugSnapshot *escaped=NULL;XirVmDebugExceptionLocal saved={0};uint32_t stops=0;
    for (uint32_t polls=0;polls<64;++polls) {
        XdapCurrentResult result={0};XrXirValueStatus polled=xdap_current_poll(fixture.launch->control,1000,&result);
        if (polled!=XR_XIR_VALUE_OK) fprintf(stderr,"selector poll=%u state=%u stop%u\n",polled,result.state,stops);
        CHECK(polled==XR_XIR_VALUE_OK);
        if (result.state==XDAP_CURRENT_STOPPED) {
            CHECK(stops<expected->stops && fixture.length==strlen(expected->prefix[stops]));
            CHECK(!memcmp(fixture.output,expected->prefix[stops],fixture.length));
            XrXirVmDebugSnapshot *snapshot=NULL;CHECK(xdap_current_snapshot(fixture.launch->control,&snapshot)==XR_XIR_VALUE_OK);
            XirVmDebugExceptionLocal local={0};CHECK(xir_vm_debug_snapshot_exception_local(snapshot,&local)==XR_XIR_VALUE_OK);
            XrXirVmDebugFrameInfo frame={0};XrXirValue name={0};
            CHECK(xr_xir_vm_debug_snapshot_frame(snapshot,0,&frame,&name)==XR_XIR_VALUE_OK);xr_xir_value_drop(&name);
            CHECK(local.function==frame.function);
            if (expected->accepted[stops]) {
                CHECK(local.kind==XIR_VM_DEBUG_EXCEPTION_LOCAL_MATCH && local.handler);
                bool found=false;
                for (uint32_t i=0;i<count;++i) if (accepted[i].function==local.function && accepted[i].instruction==local.accepted)
                    found=accepted_lines[i]==handler_line(expected->text,expected->accepted[stops]);
                CHECK(found);
            } else CHECK(local.kind==XIR_VM_DEBUG_EXCEPTION_LOCAL_PROPAGATE && !local.handler && local.accepted==UINT32_MAX);
            if (!escaped) { escaped=snapshot;saved=local; } else xr_xir_vm_debug_snapshot_drop(snapshot);
            ++stops;CHECK(xdap_current_continue(fixture.launch->control,result.debug_epoch,XR_XIR_VM_DEBUG_CONTINUE)==XR_XIR_VALUE_OK);
        } else if (result.state==XDAP_CURRENT_ENDED) {
            if (result.outcome.status!=XR_XIR_CALL_RETURNED) fprintf(stderr,"selector outcome=%u error=%lld stops%u\n",result.outcome.status,(long long)result.outcome.value.payload,stops);
            CHECK(result.outcome.status==XR_XIR_CALL_RETURNED);xdap_current_result_drop(&result);break;
        }
        xdap_current_result_drop(&result);CHECK(polls<63);
    }
    CHECK(stops==(observe ? expected->stops : 0) && fixture.length==strlen(expected->output) && !memcmp(fixture.output,expected->output,fixture.length));
    CHECK(xdap_launch_free(fixture.launch)==XR_XIR_CALL_READY);fixture.launch=NULL;
    if (escaped) {
        XirVmDebugExceptionLocal after={0};CHECK(xir_vm_debug_snapshot_exception_local(escaped,&after)==XR_XIR_VALUE_OK);
        CHECK(after.kind==saved.kind && after.function==saved.function && after.handler==saved.handler && after.accepted==saved.accepted);
        CHECK(xir_vm_debug_snapshot_exception_local(NULL,&after)==XR_XIR_VALUE_BAD_ARGUMENT);
    }
    xr_xir_vm_debug_snapshot_drop(escaped);handler_close(&fixture);
}
static size_t selector_capture_fault(size_t fail) {
    HandlerFixture fixture={0};handler_open(&fixture);CHECK(handler_build(&fixture,selector_cases[1].text)==XR_XIR_OK);
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
typedef struct SelectorBudgetProbe { XrXirDomainBudgetStats stats;bool stopped; } SelectorBudgetProbe;
static SelectorBudgetProbe selector_budget_probe(uint64_t work,uint64_t metadata) {
    HandlerFixture fixture={0};handler_open(&fixture);xr_xir_domain_drop(fixture.domain);fixture.domain=NULL;
    XrXirDomainBudgetControls budget={1048576,1048576,work,metadata,1048576};
    CHECK(xr_xir_domain_new_budgeted(1048576,&budget,&fixture.domain)==XR_XIR_VALUE_OK);
    XrXirStatus built=handler_build(&fixture,selector_cases[1].text);
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
    CHECK(!fixture.length);SelectorBudgetProbe result={xr_xir_domain_budget_stats(fixture.domain),!denied};
    XrXirCallStatus freed=xdap_launch_free(fixture.launch);fixture.launch=NULL;
    CHECK(freed==XR_XIR_CALL_READY || (denied && freed==XR_XIR_CALL_LIMIT));handler_close(&fixture);return result;
}
static SelectorBudgetProbe selector_budget_case(uint64_t work,uint64_t metadata,bool success) {
    SelectorBudgetProbe result=selector_budget_probe(work,metadata);
    if (result.stopped!=success) fprintf(stderr,"selector budget work=%llu metadata=%llu success=%u stopped=%u used=%llu peak=%llu\n",
        (unsigned long long)work,(unsigned long long)metadata,(unsigned)success,(unsigned)result.stopped,
        (unsigned long long)result.stats.work,(unsigned long long)result.stats.metadata_peak);
    CHECK(result.stopped==success);return result;
}
static void selector_budgets(void) {
    SelectorBudgetProbe baseline=selector_budget_case(128000000,1048576,true);
    CHECK(baseline.stats.work && baseline.stats.metadata_peak);
    /* A completed stop may precede deferred observer maintenance. Discover the
     * actual publication threshold instead of treating all later work as needed. */
    uint64_t low=1,high=baseline.stats.work;CHECK(!selector_budget_probe(low,1048576).stopped);
    while (high-low>1) {
        uint64_t middle=low+(high-low)/2;
        if (selector_budget_probe(middle,1048576).stopped) high=middle;else low=middle;
    }
    selector_budget_case(high,1048576,true);selector_budget_case(high-1,1048576,false);
    selector_budget_case(128000000,baseline.stats.metadata_peak,true);selector_budget_case(128000000,baseline.stats.metadata_peak-1,false);
    printf("Selector observer exact/minus-one work=%llu metadata=%llu no handler output/physical0/0 PASS\n",
        (unsigned long long)high,(unsigned long long)baseline.stats.metadata_peak);
}
int main(int argc,char **argv) {
    if (argc==3 && (!strcmp(argv[1],"--case") || !strcmp(argv[1],"--plain-case"))) {
        char *end=NULL;unsigned long which=strtoul(argv[2],&end,10);
        CHECK(end && end!=argv[2] && !*end && which<sizeof(selector_cases)/sizeof(*selector_cases));
        fprintf(stderr,"selector case%lu observe=%u\n",which,(unsigned)(!strcmp(argv[1],"--case")));
        selector_case(&selector_cases[which],which==5,!strcmp(argv[1],"--case"));return 0;
    }
    if (argc==2 && !strcmp(argv[1],"--budgets")) { selector_budgets();return 0; }
    xr_test_suppress_dialogs();bool resources=argc==2 && !strcmp(argv[1],"--resources");CHECK(argc==1 || resources);
    for (uint32_t i=0;i<sizeof(selector_cases)/sizeof(*selector_cases);++i) {
        fprintf(stderr,"selector case%u observe=1\n",i);selector_case(&selector_cases[i],i==5,true);
    }
    selector_budgets();
    size_t sites=selector_capture_fault(SIZE_MAX);CHECK(sites);
    if (resources) for (size_t fail=0;fail<sites;++fail) selector_capture_fault(fail);
    else { selector_capture_fault(0);selector_capture_fault(sites-1); }
    printf("Closed selectors: type/variant/range/literals, alternative PHI, nested/rethrow, generic identities, catch-all/cleanup, local propagation; capture sites=%zu injection=%s physical0/0 PASS\n",
        sites,resources ? "ALL" : "FIRST_LAST_ONLY");return 0;
}
