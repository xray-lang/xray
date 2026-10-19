/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_mixed_iterator_pull_prefixes.c - Control authentic pulls at every public poll boundary
 */
#include "xir_mixed_iterator_pull_resources.h"

typedef struct IteratorPrefixSpec { unsigned backend,kind,victim,mode,prefix; } IteratorPrefixSpec;
typedef struct IteratorPrefixResult {
    XrXirDomainBudgetStats cost;
    unsigned polls,linked,terminal,root_terminal,waits,busy,control_status;
    size_t sites;
} IteratorPrefixResult;

static unsigned iterator_prefix_entry(unsigned kind) { CHECK(kind<4);return kind==0?1u:kind==1?2u:3u; }
static XrXirInstanceResult iterator_prefix_step(IteratorResourceRun *run,XrXirInstanceResult previous) {
    if(previous.outcome.status==XR_XIR_CALL_SUSPENDED) iterator_resume(&run->traces[run->current],previous);
    XrXirInstanceResult event=iterator_resource_tick(run);CHECK(run->complete);
    if(event.outcome.status==XR_XIR_CALL_SUSPENDED) {
        XrXirWaitRequest request={0};
        CHECK(xr_xir_instance_wait_request(run->instance,event.epoch,event.outcome.wake,&request)==XR_XIR_CALL_READY);
        CHECK(request.kind==XR_XIR_WAIT_TIMER_MS && request.after_ms==7 && !request.reserved);++run->waits;
    } else CHECK(event.outcome.status==XR_XIR_CALL_READY || event.outcome.status==XR_XIR_CALL_RETURNED || event.outcome.status==XR_XIR_CALL_RUNTIME_PANIC);
    return event;
}
static bool iterator_prefix_finished(XrXirInstanceResult event) {
    return event.outcome.status!=XR_XIR_CALL_READY && event.outcome.status!=XR_XIR_CALL_SUSPENDED;
}
static void iterator_prefix_returned(IteratorResourceRun *run,unsigned kind,XrXirInstanceResult event) {
    CHECK(xr_xir_call_result_valid(&event.outcome));
    if(kind==3) {
        CHECK(event.outcome.status==XR_XIR_CALL_RUNTIME_PANIC && event.outcome.panic.detail.code==XR_XIR_PANIC_ITERATOR_EXHAUSTED);
        return;
    }
    CHECK(event.outcome.status==XR_XIR_CALL_RETURNED);
    if(kind==1) CHECK(event.outcome.value.type==XR_XIR_BOOL && event.outcome.value.payload==1);
    else {
        iterator_text(&event.outcome.value,"birch");
        CHECK(xr_xir_value_copy(&event.outcome.value,&run->traces[run->current].held[1])==XR_XIR_VALUE_OK);
    }
}
static void iterator_prefix_cancel(IteratorResourceRun *run,const IteratorPrefixSpec *spec,IteratorPrefixResult *result,XrXirInstanceResult before) {
    unsigned at=spec->victim,peer=1-at;XirGenerator *g=(XirGenerator *)run->addresses[at];
    XirGenerator *other=(XirGenerator *)run->addresses[peer];XrXirCall *peer_call=other->call;
    uint64_t peer_polls=other->accounting.polls,polls=g->accounting.polls;
    XrXirTaskWaitToken old={0};bool token=xr_xir_call_iterator_wait_token(run->instance->call,&old);
    XrXirCallStatus status=xr_xir_instance_cancel_current(run->instance);result->control_status=status;
    CHECK(status==(result->root_terminal?XR_XIR_CALL_BAD_STATE:XR_XIR_CALL_CANCEL_REQUESTED));
    if(before.outcome.status==XR_XIR_CALL_SUSPENDED)
        CHECK(xr_xir_instance_resume(run->instance,before.epoch,before.outcome.wake)==XR_XIR_CALL_BAD_STATE);
    if(token) CHECK(xr_xir_call_complete_iterator_wait(run->instance->call,&old)==XR_XIR_CALL_BAD_STATE);
    if(result->root_terminal) {
        XrXirInstanceResult after=iterator_resource_tick(run);CHECK(after.outcome.status==before.outcome.status);
        if(spec->kind==3) CHECK(after.outcome.panic.detail.code==before.outcome.panic.detail.code);
        else if(spec->kind==1) CHECK(after.outcome.value.type==XR_XIR_BOOL && after.outcome.value.payload==1);
        else iterator_text(&after.outcome.value,"birch");
    } else {
        XrXirValue empty={0};CHECK(iterator_resource_return(run,XR_XIR_CALL_CANCELLED,&empty));
    }
    if(result->terminal) CHECK(g->outcome.status==XR_XIR_CALL_RETURNED && run->cleanup[at]==1);
    else if(result->linked) CHECK(g->outcome.status==XR_XIR_CALL_CANCELLED && run->cleanup[at]==1);
    else CHECK(g->call && g->accounting.polls==polls && !run->cleanup[at]);
    CHECK(other->call==peer_call && other->accounting.polls==peer_polls && !other->consumer && !run->cleanup[peer]);
    CHECK(iterator_pull_second(run,peer,false) && iterator_resource_end(run,peer));
    CHECK(other->outcome.status==XR_XIR_CALL_RETURNED && run->cleanup[peer]==1);
}
/* Expected producer outcomes are frozen before control, independently of later cleanup. */
static void iterator_prefix_control(IteratorResourceRun *run,const IteratorPrefixSpec *spec,IteratorPrefixResult *result,XrXirInstanceResult event) {
    XirGenerator *g=(XirGenerator *)run->addresses[spec->victim];
    result->root_terminal=iterator_prefix_finished(event);
    result->terminal=atomic_load_explicit(&g->state,memory_order_acquire)==XIR_GENERATOR_TERMINAL;
    result->linked=g->consumer==run->instance->call;
    CHECK(!result->terminal || g->outcome.status==XR_XIR_CALL_RETURNED);
    XrXirTaskWaitToken old={0};bool token=xr_xir_call_iterator_wait_token(run->instance->call,&old);
    if(result->root_terminal) iterator_prefix_returned(run,spec->kind,event);
    if(!spec->mode) iterator_prefix_cancel(run,spec,result,event);
    else {
        XrXirCallStatus status=spec->mode==1?xr_xir_instance_stop(run->instance):xr_xir_instance_free(run->instance);
        XrXirCallStatus expected=spec->mode==1 && result->root_terminal?event.outcome.status:XR_XIR_CALL_READY;
        result->control_status=status;
        if(status!=expected)fprintf(stderr,"Iterator prefix control backend=%u kind=%u victim=%u mode=%u prefix=%u actual=%u expected=%u rootTerminal=%u\n",spec->backend,spec->kind,spec->victim,spec->mode,spec->prefix,status,expected,result->root_terminal);
        CHECK(status==expected);if(spec->mode==2)run->instance=NULL;
        if(run->instance) {
            XrXirInstanceResult after=iterator_resource_tick(run);
            CHECK(after.outcome.status==(result->root_terminal?event.outcome.status:XR_XIR_CALL_CANCELLED));
            if(result->root_terminal && spec->kind<3) {
                if(spec->kind==1) CHECK(after.outcome.value.type==XR_XIR_BOOL && after.outcome.value.payload==1);
                else iterator_text(&after.outcome.value,"birch");
            }
        }
        for(unsigned i=0;i<2;++i) {
            XirGenerator *p=(XirGenerator *)run->addresses[i];
            CHECK(p->outcome.status==((i==spec->victim && result->terminal)?XR_XIR_CALL_RETURNED:XR_XIR_CALL_CANCELLED));
            CHECK(!p->call && !p->consumer && run->cleanup[i]==1);
        }
    }
    if(run->instance && event.outcome.status==XR_XIR_CALL_SUSPENDED)
        CHECK(xr_xir_instance_resume(run->instance,event.epoch,event.outcome.wake)==XR_XIR_CALL_BAD_STATE);
    if(run->instance && token) CHECK(xr_xir_call_complete_iterator_wait(run->instance->call,&old)==XR_XIR_CALL_BAD_STATE);
}
static void iterator_prefix_late(IteratorResourceRun *run,const XrXirCallStatus expected[2]) {
    CHECK(run->fixture.releases==1 && !run->fixture.vm && !run->fixture.entries);
    for(unsigned i=0;i<2;++i) {
        const XirGenerator *g=(const XirGenerator *)(uintptr_t)run->traces[i].iterator.payload;
        CHECK(xr_xir_value_valid(&run->traces[i].iterator) && !g->executor && !g->call && !g->consumer && g->outcome.status==expected[i]);
        size_t attempts=runtime_attempts;xr_xir_value_drop(&run->traces[i].iterator);
        CHECK(iterator_resource_generator_owner[i]==run->addresses[i] && iterator_physical(run->addresses[i]));
        xr_xir_value_drop(&run->traces[i].alias);CHECK(runtime_attempts==attempts);
        CHECK(!iterator_resource_generator_owner[i] && iterator_resource_generator_frees[i]==1 && iterator_cell_freed[i]==1 && !iterator_cell_owner[i]);
    }
    CHECK(!effects_compile_live && !effects_compile_bytes);
    for(unsigned i=0;i<2;++i) for(unsigned j=0;j<2;++j) if(run->traces[i].held[j].type) {
        iterator_text(&run->traces[i].held[j],j?"birch":"cedar");xr_xir_value_drop(&run->traces[i].held[j]);
    }
}
static IteratorPrefixResult iterator_prefix_operation(const IteratorPrefixSpec *spec) {
    CHECK(spec->backend<4 && spec->kind<4 && spec->victim<2 && spec->mode<4);
    CHECK(!iterator_resource_borrowed && !runtime_live && !effects_compile_live);runtime_fail_at=SIZE_MAX;runtime_attempts=0;
    memset(iterator_cell_owner,0,sizeof(iterator_cell_owner));memset(iterator_cell_freed,0,sizeof(iterator_cell_freed));
    memset(iterator_resource_generator_owner,0,sizeof(iterator_resource_generator_owner));memset(iterator_resource_generator_frees,0,sizeof(iterator_resource_generator_frees));
    iterator_resource_final=(XrXirDomainBudgetStats){0};iterator_resource_previous=(XrXirDomainBudgetStats){0};iterator_resource_disposals=0;
    IteratorResourceRun run={.fault=XR_XIR_CALL_OOM,.complete=true,.pull=1};IteratorPrefixResult result={0};
    XrXirProgram *program=iterator_program(&run.fixture,spec->backend);CHECK(!runtime_attempts && !runtime_live && !runtime_bytes);
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,iterator_resource_output,&run};
    CHECK(xr_xir_instance_new(program,&config,&run.instance)==XR_XIR_CALL_READY);
    run.had_domain=true;run.domain=run.instance->domain;iterator_resource_borrowed=run.domain;
    for(unsigned i=0;i<2;++i){run.traces[i].instance=run.instance;run.traces[i].fixture=&run.fixture;run.traces[i].index=i;}
    xr_xir_compile_program_drop(program);CHECK(!run.fixture.releases && iterator_resource_make(&run));
    for(unsigned i=0;i<2;++i) CHECK(iterator_pull_first(&run,i));
    for(unsigned i=0;i<2;++i) {
        CHECK(run.traces[i].witness.root==(iterator_native(spec->backend,true)?2u:1u));
        CHECK(run.traces[i].witness.producer==(iterator_native(spec->backend,false)?2u:1u));
    }
    run.index=spec->kind==3?1:0;CHECK(iterator_resource_start(&run,spec->victim,iterator_prefix_entry(spec->kind)));
    XrXirInstanceResult event={0};event.outcome.status=XR_XIR_CALL_READY;
    while(spec->mode==3?!iterator_prefix_finished(event):result.polls<spec->prefix) {
        CHECK(!iterator_prefix_finished(event) && result.polls<4096);
        event=iterator_prefix_step(&run,event);++result.polls;
    }
    XrXirCallStatus expected[2];
    for(unsigned i=0;i<2;++i) {
        XirGenerator *g=(XirGenerator *)run.addresses[i];
        expected[i]=atomic_load_explicit(&g->state,memory_order_acquire)==XIR_GENERATOR_TERMINAL?g->outcome.status:XR_XIR_CALL_CANCELLED;
        if(!spec->mode && i!=spec->victim)expected[i]=XR_XIR_CALL_RETURNED;
    }
    if(spec->mode==3)iterator_prefix_returned(&run,spec->kind,event);
    else iterator_prefix_control(&run,spec,&result,event);
    iterator_resource_retire(&run);CHECK(run.complete);iterator_prefix_late(&run,expected);
    CHECK(!iterator_resource_borrowed && iterator_resource_disposals==1 && run.cleanup[0]==1 && run.cleanup[1]==1 && run.busy==8);
    CHECK(!runtime_live && !runtime_bytes && !effects_compile_live && !effects_compile_bytes);
    result.cost=iterator_resource_final;result.sites=runtime_attempts;result.waits=run.waits;result.busy=run.busy;
    if(spec->mode<3)printf("Iterator pull prefix backend=%u kind=%u victim=%u mode=%u prefix=%u linked=%u terminal=%u rootTerminal=%u controlStatus=%u sites=%zu value=%llu call=%llu work=%llu waits=%u noExtraRetain originalDomain physical0/0\n",spec->backend,spec->kind,spec->victim,spec->mode,spec->prefix,result.linked,result.terminal,result.root_terminal,result.control_status,result.sites,(unsigned long long)result.cost.requested_bytes,(unsigned long long)result.cost.requested_call_bytes,(unsigned long long)result.cost.work,result.waits);
    return result;
}
int main(int argc,char **argv) {
    CHECK(argc==2);(void)iterator_graph;(void)iterator_resource_baseline;(void)iterator_resource_axes;
    const char *names[]={"0","1","vm","native"};unsigned backend=0;while(backend<4 && strcmp(argv[1],names[backend]))++backend;CHECK(backend<4);
    for(unsigned kind=0;kind<4;++kind) for(unsigned victim=0;victim<2;++victim) {
        IteratorPrefixSpec spec={backend,kind,victim,3,0};IteratorPrefixResult normal=iterator_prefix_operation(&spec);CHECK(normal.polls==(kind==3?34u:22u));
        unsigned linked=0,terminal=0,root_terminal=0;
        for(unsigned prefix=0;prefix<=normal.polls;++prefix) for(unsigned mode=0;mode<3;++mode) {
            spec.mode=mode;spec.prefix=prefix;IteratorPrefixResult result=iterator_prefix_operation(&spec);
            CHECK(result.polls==prefix);linked+=result.linked;terminal+=result.terminal;root_terminal+=result.root_terminal;
        }
        CHECK(linked==(kind==3?90u:48u) && terminal==(kind==3?9u:0u) && root_terminal==3);
        printf("Iterator pull prefix census backend=%u kind=%u victim=%u polls=%u controls=%u linked=%u terminal=%u rootTerminal=%u original1quantum physical0/0\n",backend,kind,victim,normal.polls,(normal.polls+1)*3,linked,terminal,root_terminal);
    }
    return 0;
}
