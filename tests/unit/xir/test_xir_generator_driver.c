/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_generator_driver.c - Independent pull, wait and cancellation witnesses
 *
 * KEY CONCEPT:
 *   Two producers keep separate frames and transfer each owned value once.
 */
#include "xir/xxir_call_generator.h"
#include "xir/xxir_type_arena.h"
#include "xir/xxir_types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if (!(c)) {fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}} while (0)
#include "xir_source_fixture_owner.h"
#include "xir_runtime_allocations.h"

typedef struct GeneratorWitness {uint32_t mode,dispatches,cleanups,releases;XrXirType element;} GeneratorWitness;
typedef struct GeneratorState {uint32_t step;} GeneratorState;
typedef struct GeneratorFixture {
    SourceFixtureOwner compiler;XrXirTypeArena *arena;XrXirDomain *domain;
    XrXirCallEntry entry;XrXirCallConfig config;XrXirCallAccounting accounting;
    GeneratorWitness witness;XrXirType parameter;
} GeneratorFixture;
static XrXirAction producer(XrXirCallView *view) {
    GeneratorWitness *w=view->instance;GeneratorState *s=view->state;++w->dispatches;
    CHECK(xr_xir_call_poll_bounded(view->activation,1).status==XR_XIR_CALL_BUSY);
    CHECK(xr_xir_call_free(view->activation)==XR_XIR_CALL_BUSY);
    XrXirValue empty={0};CHECK(xr_xir_call_generator_take(view->activation,&empty)==XR_XIR_CALL_BUSY);
    if (view->phase==XR_XIR_CALL_EXIT) {
        CHECK(!w->cleanups);++w->cleanups;
        CHECK(xr_xir_call_cleanup_frontier(view,0)==XR_XIR_CALL_READY);
        return (XrXirAction){.kind=XR_XIR_ACTION_EXIT_DONE};
    }
    uint32_t step=s->step++;
    if (!step) CHECK(xr_xir_call_cleanup_frontier(view,1)==XR_XIR_CALL_READY);
    if (w->mode==1 && step==1) return (XrXirAction){.kind=XR_XIR_ACTION_SUSPEND};
    if (step>=(w->mode==1 ? 3u : 2u)) return (XrXirAction){.kind=XR_XIR_ACTION_RETURN};
    XrXirValue value=w->element==XR_XIR_UNIT ? (XrXirValue){0} :
        w->element==XR_XIR_STRING ? view->arguments[0] : (XrXirValue){XR_XIR_I64,0,step ? 22 : 11};
    if (w->mode==2) CHECK(xr_xir_call_request_cancel(view->activation)==XR_XIR_CALL_CANCEL_REQUESTED);
    if (w->mode==3) value=(XrXirValue){XR_XIR_BOOL,0,1};
    if (w->mode==4) return (XrXirAction){.kind=XR_XIR_ACTION_GENERATOR_YIELD,.value=value,.flags=1};
    if (w->mode==6) return (XrXirAction){.kind=XR_XIR_ACTION_RETURN,.value=value};
    return (XrXirAction){.kind=XR_XIR_ACTION_GENERATOR_YIELD,.value=value};
}
static void producer_release(XrXirCallView *view,XrXirCallStatus reason) {
    GeneratorWitness *w=view->instance;CHECK(!w->releases);++w->releases;
    CHECK(reason==XR_XIR_CALL_RETURNED || reason==XR_XIR_CALL_CANCELLED || reason==XR_XIR_CALL_LIMIT || reason==XR_XIR_CALL_BAD_STATE);
}
static void fixture_new(GeneratorFixture *f,XrXirType element,uint32_t mode) {
    *f=(GeneratorFixture){0};source_fixture_owner_new(&f->compiler);
    XrXirTypeNode nodes[]={
        {.kind=XR_XIR_TYPE_ITERATOR,.element=XR_XIR_I64},
        {.kind=XR_XIR_TYPE_ITERATOR,.element=XR_XIR_UNIT},
        {.kind=XR_XIR_TYPE_ITERATOR,.element=XR_XIR_STRING},
        {.kind=XR_XIR_TYPE_CELL,.element=XR_XIR_I64}};
    XrXirTypes types={nodes,4,NULL,NULL};
    CHECK(xr_xir_compile_type_arena_new(&f->compiler.context,&types,&f->arena)==XR_XIR_VALUE_OK);
    const XrXirDomainBudgetControls controls={1048576,8388608,1000000,1048576,1048576};
    CHECK(xr_xir_domain_new_budgeted(UINT64_C(1048576),&controls,&f->domain)==XR_XIR_VALUE_OK);
    f->witness=(GeneratorWitness){.mode=mode,.element=element};f->parameter=XR_XIR_STRING;
    f->entry=(XrXirCallEntry){.abi_version=XR_XIR_CALL_ABI_VERSION,
        .result=(XrXirType)(element==XR_XIR_I64 ? 256 : element==XR_XIR_UNIT ? 257 : 258),
        .state_bytes=sizeof(GeneratorState),.resume=producer,.release=producer_release,
        .flags=XR_XIR_ENTRY_GENERATOR|XR_XIR_ENTRY_EXIT};
    if (element==XR_XIR_STRING) {f->entry.parameters=&f->parameter;f->entry.parameter_count=1;}
    CHECK(xr_xir_call_config_init(&f->config,sizeof(f->config))==XR_XIR_CALL_READY);
    f->config.entries=&f->entry;f->config.entry_count=1;f->config.instance=&f->witness;
    f->config.accounting=&f->accounting;f->config.byte_limit=1048576;f->config.poll_limit=64;
    f->config.admission=(XrXirValueAdmission){f->arena,f->domain,NULL,NULL,100000,65536};
}
static void fixture_free(GeneratorFixture *f) {
    CHECK(!f->accounting.live_bytes && !f->accounting.depth && f->accounting.allocations==f->accounting.frees);
    xr_xir_compile_type_arena_drop(f->arena);xr_xir_domain_drop(f->domain);
    source_fixture_owner_free(&f->compiler);CHECK(!runtime_live && !runtime_bytes);
}
static void yield_once(XrXirCall *call,XrXirType type,int64_t expected) {
    XrXirCallResult r=xr_xir_call_poll_bounded(call,UINT64_MAX);
    CHECK(r.status==XR_XIR_CALL_YIELDED && !r.wake && r.value.type==(uint32_t)type);
    CHECK(xr_xir_call_result_valid(&r));
    XrXirCallResult copy={0};CHECK(xr_xir_call_result_copy(&r,&copy)==XR_XIR_VALUE_OK);
    CHECK(copy.status==XR_XIR_CALL_YIELDED);xr_xir_call_result_drop(&copy);
    CHECK(xr_xir_call_poll_bounded(call,1).status==XR_XIR_CALL_YIELDED);
    XrXirWaitRequest wait={.kind=777};
    CHECK(xr_xir_call_wait_request(call,1,&wait)==XR_XIR_CALL_BAD_STATE && wait.kind==777);
    CHECK(xr_xir_call_resume(call,1)==XR_XIR_CALL_BAD_STATE);
    XrXirValue value={0};CHECK(xr_xir_call_take_result(call,&value)==XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_call_generator_take(call,&value)==XR_XIR_CALL_YIELDED);
    CHECK(value.type==(uint32_t)type);if (type==XR_XIR_I64) CHECK(value.payload==expected);
    xr_xir_value_drop(&value);CHECK(xr_xir_call_generator_take(call,&value)==XR_XIR_CALL_BAD_STATE);
}
static void interleaved(void) {
    GeneratorFixture f;fixture_new(&f,XR_XIR_I64,0);
    XrXirCallAccounting other={0};GeneratorWitness right_witness={.element=XR_XIR_I64};
    XrXirCallConfig right_config=f.config;right_config.accounting=&other;right_config.instance=&right_witness;
    XrXirCall *left=NULL,*right=NULL;
    CHECK(xr_xir_call_generator_new(&f.config,0,NULL,0,&left)==XR_XIR_CALL_READY);
    CHECK(xr_xir_call_generator_new(&right_config,0,NULL,0,&right)==XR_XIR_CALL_READY);
    CHECK(!f.witness.dispatches && !right_witness.dispatches);
    yield_once(left,XR_XIR_I64,11);yield_once(right,XR_XIR_I64,11);yield_once(left,XR_XIR_I64,22);
    CHECK(xr_xir_call_free(left)==XR_XIR_CALL_READY);
    CHECK(f.witness.cleanups==1 && f.witness.releases==1 && right_witness.cleanups==0);
    yield_once(right,XR_XIR_I64,22);
    CHECK(xr_xir_call_poll_bounded(right,UINT64_MAX).status==XR_XIR_CALL_RETURNED);
    CHECK(right_witness.cleanups==1 && right_witness.releases==1);CHECK(xr_xir_call_free(right)==XR_XIR_CALL_READY);
    CHECK(!other.live_bytes && !other.depth && other.allocations==other.frees);fixture_free(&f);
}
static void wait_and_cancel(void) {
    GeneratorFixture f;fixture_new(&f,XR_XIR_I64,1);XrXirCall *call=NULL;
    CHECK(xr_xir_call_generator_new(&f.config,0,NULL,0,&call)==XR_XIR_CALL_READY);
    yield_once(call,XR_XIR_I64,11);XrXirCallResult r=xr_xir_call_poll_bounded(call,99);
    CHECK(r.status==XR_XIR_CALL_SUSPENDED && r.wake);XrXirWaitRequest wait={0};
    CHECK(xr_xir_call_wait_request(call,r.wake,&wait)==XR_XIR_CALL_READY && wait.kind==XR_XIR_WAIT_YIELD);
    CHECK(xr_xir_call_poll_bounded(call,99).status==XR_XIR_CALL_SUSPENDED);
    CHECK(xr_xir_call_resume(call,r.wake+1)==XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_call_resume(call,r.wake)==XR_XIR_CALL_READY);yield_once(call,XR_XIR_I64,22);
    CHECK(xr_xir_call_free(call)==XR_XIR_CALL_READY);CHECK(f.witness.cleanups==1);fixture_free(&f);
    fixture_new(&f,XR_XIR_I64,2);call=NULL;
    CHECK(xr_xir_call_generator_new(&f.config,0,NULL,0,&call)==XR_XIR_CALL_READY);
    CHECK(xr_xir_call_poll_bounded(call,99).status==XR_XIR_CALL_CANCELLED);
    CHECK(f.witness.cleanups==1 && f.witness.releases==1);CHECK(xr_xir_call_free(call)==XR_XIR_CALL_READY);fixture_free(&f);
}
static void unstarted_and_unit(void) {
    GeneratorFixture f;fixture_new(&f,XR_XIR_UNIT,0);XrXirCall *call=NULL;
    CHECK(xr_xir_call_generator_new(&f.config,0,NULL,0,&call)==XR_XIR_CALL_READY);
    CHECK(xr_xir_call_free(call)==XR_XIR_CALL_READY);
    CHECK(!f.witness.dispatches && !f.witness.cleanups && f.witness.releases==1);fixture_free(&f);
    fixture_new(&f,XR_XIR_UNIT,0);call=NULL;
    CHECK(xr_xir_call_generator_new(&f.config,0,NULL,0,&call)==XR_XIR_CALL_READY);
    yield_once(call,XR_XIR_UNIT,0);yield_once(call,XR_XIR_UNIT,0);
    CHECK(xr_xir_call_poll_bounded(call,99).status==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_call_free(call)==XR_XIR_CALL_READY);fixture_free(&f);
}
static void admission_rejects(void) {
    GeneratorFixture f;fixture_new(&f,XR_XIR_I64,0);XrXirCall *call=NULL;size_t before=runtime_attempts;
    CHECK(xr_xir_call_new(&f.config,0,NULL,0,&call)==XR_XIR_CALL_BAD_ARGUMENT && !call);
    f.entry.flags=XR_XIR_ENTRY_EXIT;
    CHECK(xr_xir_call_generator_new(&f.config,0,NULL,0,&call)==XR_XIR_CALL_BAD_ARGUMENT && !call);
    f.entry.flags=XR_XIR_ENTRY_GENERATOR;f.entry.result=XR_XIR_I64;
    CHECK(xr_xir_call_generator_new(&f.config,0,NULL,0,&call)==XR_XIR_CALL_BAD_ARGUMENT && !call);
    f.entry.result=(XrXirType)256;f.entry.abi_version=29;
    CHECK(xr_xir_call_generator_new(&f.config,0,NULL,0,&call)==XR_XIR_CALL_BAD_ABI && !call);
    f.entry.abi_version=XR_XIR_CALL_ABI_VERSION;f.parameter=(XrXirType)259;f.entry.parameters=&f.parameter;f.entry.parameter_count=1;
    XrXirValue external={XR_XIR_I64,0,1};
    CHECK(xr_xir_call_generator_new(&f.config,0,&external,1,&call)==XR_XIR_CALL_BAD_ARGUMENT && !call);
    CHECK(runtime_attempts==before && !f.witness.dispatches);fixture_free(&f);
}
static void owned_string(void) {
    for (uint32_t held=0;held<2;++held) {
        GeneratorFixture f;fixture_new(&f,XR_XIR_STRING,0);XrXirCall *call=NULL;XrXirValue input={0};
        CHECK(xr_xir_string_new(f.domain,"independent owner",17,&input)==XR_XIR_VALUE_OK);
        CHECK(xr_xir_call_generator_new(&f.config,0,&input,1,&call)==XR_XIR_CALL_READY);
        xr_xir_value_drop(&input);
        XrXirCallResult result=xr_xir_call_poll_bounded(call,99),copy={0};
        CHECK(result.status==XR_XIR_CALL_YIELDED && xr_xir_call_result_copy(&result,&copy)==XR_XIR_VALUE_OK);
        XrXirValue occupied={XR_XIR_I64,0,73};
        CHECK(xr_xir_call_generator_take(call,&occupied)==XR_XIR_CALL_BAD_ARGUMENT && occupied.payload==73);
        XrXirCallResult terminal={0};CHECK(xr_xir_call_take_outcome(call,&terminal)==XR_XIR_CALL_BAD_STATE);
        XrXirValue taken={0};
        if (!held) CHECK(xr_xir_call_generator_take(call,&taken)==XR_XIR_CALL_YIELDED);
        CHECK(xr_xir_call_free(call)==XR_XIR_CALL_READY && f.witness.cleanups==1 && f.witness.releases==1);
        const char *bytes=NULL;size_t length=0;
        CHECK(xr_xir_string_view(&copy.value,&bytes,&length) && length==17 && !memcmp(bytes,"independent owner",17));
        if (!held) CHECK(xr_xir_string_view(&taken,&bytes,&length) && length==17 && !memcmp(bytes,"independent owner",17));
        xr_xir_call_result_drop(&copy);xr_xir_value_drop(&taken);fixture_free(&f);
    }
}
static void malformed_actions(void) {
    const uint32_t modes[]={3,4,5,6};
    for (uint32_t i=0;i<sizeof(modes)/sizeof(modes[0]);++i) {
        GeneratorFixture f;fixture_new(&f,XR_XIR_I64,modes[i]);XrXirCall *call=NULL;
        if (modes[i]==5) {
            f.entry.flags=XR_XIR_ENTRY_EXIT;
            CHECK(xr_xir_call_new(&f.config,0,NULL,0,&call)==XR_XIR_CALL_READY);
        } else CHECK(xr_xir_call_generator_new(&f.config,0,NULL,0,&call)==XR_XIR_CALL_READY);
        XrXirCallResult result=xr_xir_call_poll_bounded(call,99);
        CHECK(result.status==XR_XIR_CALL_BAD_STATE && !result.wake && !result.value.type);
        CHECK(f.witness.releases==1 && !f.witness.cleanups);
        CHECK(xr_xir_call_free(call)==XR_XIR_CALL_READY);fixture_free(&f);
    }
}
static void constructor_boundaries(void) {
    uint64_t exact_live=0,exact_requested=0;
    for (unsigned pass=0;pass<4;++pass) {
        GeneratorFixture f;fixture_new(&f,XR_XIR_I64,0);XrXirCall *call=NULL;
        XrXirCallBudget budget={.byte_limit=pass ? exact_live-(pass==2) : 1048576,
            .requested_limit=pass ? exact_requested-(pass==3) : 8388608,.resume_limit=64,.work_domain=f.domain};
        XrXirCallRequest request={0,NULL,0,NULL,NULL};
        XrXirCallStatus status=xr_xir_call_generator_new_budgeted(&f.config,&request,&budget,&call);
        CHECK(status==(pass<2 ? XR_XIR_CALL_READY : XR_XIR_CALL_LIMIT));
        if (call) CHECK(xr_xir_call_free(call)==XR_XIR_CALL_READY);
        if (!pass) {exact_live=budget.peak_bytes;exact_requested=budget.requested_bytes;CHECK(exact_live && exact_requested);}
        CHECK(!budget.live_bytes && budget.allocations==budget.frees && !budget.observed_calls && !f.witness.dispatches);
        fixture_free(&f);
    }
}
static size_t constructor_trial(GeneratorFixture *f,const XrXirValue *input,size_t fault,XrXirCallBudget *budget) {
    f->witness.dispatches=f->witness.cleanups=f->witness.releases=0;
    size_t before_live=runtime_live,before_bytes=runtime_bytes;
    runtime_attempts=0;runtime_fail_at=fault;XrXirCall *call=NULL;
    XrXirCallRequest request={0,input,1,NULL,NULL};
    XrXirCallStatus status=budget ? xr_xir_call_generator_new_budgeted(&f->config,&request,budget,&call) :
        xr_xir_call_generator_new(&f->config,0,input,1,&call);
    size_t sites=runtime_attempts;runtime_fail_at=SIZE_MAX;
    if (fault==SIZE_MAX) CHECK(status==XR_XIR_CALL_READY && call && !f->witness.dispatches);
    else CHECK(status==XR_XIR_CALL_OOM && !call && sites==fault+1 && !f->witness.dispatches);
    if (call) CHECK(xr_xir_call_free(call)==XR_XIR_CALL_READY);
    CHECK(runtime_live==before_live && runtime_bytes==before_bytes);
    CHECK(!f->accounting.live_bytes && !f->accounting.depth && f->accounting.allocations==f->accounting.frees);
    if (budget) CHECK(!budget->live_bytes && budget->allocations==budget->frees && !budget->observed_calls);
    return sites;
}
static void constructor_faults(void) {
    for (unsigned shared=0;shared<2;++shared) {
        GeneratorFixture f;fixture_new(&f,XR_XIR_STRING,0);XrXirValue input={0};
        CHECK(xr_xir_string_new(f.domain,"retained",8,&input)==XR_XIR_VALUE_OK);
        XrXirCallBudget owner={.byte_limit=1048576,.requested_limit=8388608,.resume_limit=64,.work_domain=f.domain};
        XrXirCallBudget *budget=shared ? &owner : NULL;
        size_t sites=constructor_trial(&f,&input,SIZE_MAX,budget);CHECK(sites && sites<100);
        for (size_t fault=0;fault<sites;++fault) {
            uint64_t requests=owner.requested_bytes;
            constructor_trial(&f,&input,fault,budget);
            CHECK(constructor_trial(&f,&input,SIZE_MAX,budget)==sites);
            if(shared) CHECK(owner.requested_bytes>requests);
        }
        XrXirDomainBudgetStats stats=xr_xir_domain_budget_stats(f.domain);
        CHECK(stats.bound && stats.work_limit==1000000 && stats.requested_call_limit==8388608 && !stats.call_live);
        xr_xir_value_drop(&input);fixture_free(&f);
        printf("Generator constructor shared=%u allocation sites=%zu; same finite Domain retries and physical baseline PASS\n",shared,sites);
    }
}
int main(int argc,char **argv) {
    if (argc==2 && !strcmp(argv[1],"--faults")) {constructor_faults();return 0;}
    CHECK(argc==1);
    admission_rejects();unstarted_and_unit();interleaved();wait_and_cancel();owned_string();malformed_actions();constructor_boundaries();
    puts("Common Call pull and scheduling wait separated; independent frames, once transfer, reentry and cleanup PASS");return 0;
}
