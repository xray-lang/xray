/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_mixed_iterator_index_boundaries.c - Real indexed pulls preserve bounds and sticky exhaustion
 */
#include "xir_mixed_iterator_pull_resources.h"

typedef struct IteratorIndexSpec { unsigned backend,victim,stage,maximum; } IteratorIndexSpec;
static unsigned iterator_index_bounds,iterator_index_sticky,iterator_index_stale;

/* A rejected negative index must not consume a cached value or enter the producer. */
static bool iterator_index_negative(IteratorResourceRun *run,unsigned at,int64_t index) {
    XirGenerator *g=(XirGenerator *)run->addresses[at];XrXirCall *call=g->call;
    XrXirCallAccounting accounting=g->accounting;XrXirCallResult outcome=g->outcome,cached={0};
    const CallFrame *frame=call?call->top:NULL;if(call) cached=call->result;
    XirGeneratorState state=atomic_load_explicit(&g->state,memory_order_acquire);
    unsigned cleanup=run->cleanup[at];run->index=index;
    if(!iterator_resource_start(run,at,3)) return false;
    XrXirInstanceResult event={0};unsigned steps=0;
    do {event=iterator_resource_tick(run);CHECK(++steps<4096);} while(run->complete && event.outcome.status==XR_XIR_CALL_READY);
    if(!run->complete) return false;
    CHECK(event.outcome.status==XR_XIR_CALL_BOUNDS && !event.outcome.wake && xr_xir_call_result_valid(&event.outcome));
    CHECK(event.outcome.panic.detail.code==XR_XIR_PANIC_BOUNDS && event.outcome.panic.detail.index==index && !event.outcome.panic.detail.length);
    CHECK(g->call==call && !g->consumer && !memcmp(&g->accounting,&accounting,sizeof(accounting)));
    if(call) CHECK(call->top==frame && !memcmp(&call->result,&cached,sizeof(cached)));
    CHECK(!memcmp(&g->outcome,&outcome,sizeof(outcome)) && atomic_load_explicit(&g->state,memory_order_acquire)==state);
    CHECK(run->cleanup[at]==cleanup);XrXirValue empty={0};
    CHECK(xr_xir_instance_take_result(run->instance,&empty)==XR_XIR_CALL_BAD_STATE && !empty.type && !empty.reserved && !empty.payload);
    ++iterator_index_bounds;return true;
}
static bool iterator_index_exhausted(IteratorResourceRun *run,unsigned at) {
    XirGenerator *g=(XirGenerator *)run->addresses[at];XrXirInstanceResult event={0};unsigned steps=0;
    do {event=iterator_resource_tick(run);CHECK(++steps<4096);} while(run->complete && event.outcome.status==XR_XIR_CALL_READY);
    if(!run->complete) return false;
    CHECK(event.outcome.status==XR_XIR_CALL_RUNTIME_PANIC && !event.outcome.wake && xr_xir_call_result_valid(&event.outcome));
    CHECK(event.outcome.panic.detail.code==XR_XIR_PANIC_ITERATOR_EXHAUSTED && run->cleanup[at]==1);
    CHECK(!g->call && !g->consumer && g->outcome.status==XR_XIR_CALL_RETURNED &&
        atomic_load_explicit(&g->state,memory_order_acquire)==XIR_GENERATOR_TERMINAL);
    XrXirValue empty={0};CHECK(xr_xir_instance_take_result(run->instance,&empty)==XR_XIR_CALL_BAD_STATE && !empty.type && !empty.reserved && !empty.payload);
    return true;
}
static bool iterator_index_sticky_end(IteratorResourceRun *run,unsigned at) {
    XirGenerator *g=(XirGenerator *)run->addresses[at];uint64_t polls=g->accounting.polls;
    for(unsigned n=0;n<2;++n) {
        XrXirValue more={0};
        if(!iterator_resource_start(run,at,2) || !iterator_resource_return(run,XR_XIR_CALL_RETURNED,&more)) return false;
        CHECK(more.type==XR_XIR_BOOL && !more.reserved && !more.payload);xr_xir_value_drop(&more);
        if(!iterator_resource_start(run,at,1) || !iterator_index_exhausted(run,at)) return false;
        run->index=n?INT64_MAX:0;
        if(!iterator_resource_start(run,at,3) || !iterator_index_exhausted(run,at)) return false;
        CHECK(g->accounting.polls==polls && run->cleanup[at]==1);iterator_index_sticky+=3;
    }
    return iterator_index_negative(run,at,-1) && iterator_index_negative(run,at,INT64_MIN);
}
static bool iterator_index_prepare(IteratorResourceRun *run,const IteratorIndexSpec *spec) {
    unsigned at=spec->victim;
    if(spec->stage) {
        run->pull=spec->stage==1?3u:1u;
        if(!iterator_pull_first(run,at)) return false;
    }
    if(spec->stage==3 && !iterator_pull_second(run,at,false)) return false;
    return iterator_index_negative(run,at,-1) && iterator_index_negative(run,at,INT64_MIN);
}
/* The real request counts remaining discards; Timer wakes change within one root epoch. */
static bool iterator_index_skip(IteratorResourceRun *run,const IteratorIndexSpec *spec) {
    unsigned at=spec->victim;int64_t index=spec->maximum?INT64_MAX:2;run->index=index;
    if(!iterator_resource_start(run,at,3)) return false;
    XrXirInstanceResult previous={0};XrXirTaskWaitToken old={0};
    for(unsigned element=spec->stage?1u:0u;element<2 && spec->stage<3;++element) {
        XrXirInstanceResult event;
        if(!iterator_resource_wait(run,element?7u:5u,&event)) return false;
        XrXirTaskWaitToken token={0};CHECK(xr_xir_call_iterator_wait_token(run->instance->call,&token));
        uint64_t consumed=spec->stage==1 || !spec->stage?element:0;
        CHECK(token.request.kind==XR_XIR_WAIT_ITERATOR_NTH && token.request.subject==(const void *)run->addresses[at]);
        CHECK(token.request.after_ms==(uint64_t)index-consumed);
        if(previous.epoch) {
            CHECK(previous.epoch==event.epoch && previous.outcome.wake!=event.outcome.wake);
            CHECK(xr_xir_instance_resume(run->instance,previous.epoch,previous.outcome.wake)==XR_XIR_CALL_BAD_STATE);
            CHECK(xr_xir_call_complete_iterator_wait(run->instance->call,&old)==XR_XIR_CALL_BAD_STATE);++iterator_index_stale;
        }
        previous=event;old=token;iterator_resume(&run->traces[at],event);
    }
    if(!iterator_index_exhausted(run,at)) return false;
    if(previous.epoch) {
        CHECK(xr_xir_instance_resume(run->instance,previous.epoch,previous.outcome.wake)==XR_XIR_CALL_BAD_STATE);
        CHECK(xr_xir_call_complete_iterator_wait(run->instance->call,&old)==XR_XIR_CALL_BAD_STATE);++iterator_index_stale;
    }
    return iterator_index_sticky_end(run,at);
}
static void iterator_index_drive(IteratorResourceRun *run,const IteratorIndexSpec *spec) {
    if(!iterator_resource_make(run)) return;
    unsigned peer=1-spec->victim;XirGenerator *g=(XirGenerator *)run->addresses[peer];
    XrXirCall *call=g->call;XrXirCallAccounting accounting=g->accounting;
    if(!iterator_index_prepare(run,spec) || !iterator_index_skip(run,spec)) return;
    CHECK(g->call==call && !g->consumer && !memcmp(&g->accounting,&accounting,sizeof(accounting)) && !run->cleanup[peer]);
    run->pull=1;
    if(!iterator_pull_first(run,peer) || !iterator_pull_second(run,peer,false)) return;
    (void)iterator_resource_end(run,peer);
}
static IteratorResourceResult iterator_index_operation(const IteratorIndexSpec *spec,size_t failure,const XrXirInstanceConfig *limits) {
    CHECK(spec->backend<4 && spec->victim<2 && spec->stage<4 && spec->maximum<2);
    CHECK(!iterator_resource_borrowed && !runtime_live && !effects_compile_live);runtime_fail_at=SIZE_MAX;
    memset(iterator_cell_owner,0,sizeof(iterator_cell_owner));memset(iterator_cell_freed,0,sizeof(iterator_cell_freed));
    memset(iterator_resource_generator_owner,0,sizeof(iterator_resource_generator_owner));memset(iterator_resource_generator_frees,0,sizeof(iterator_resource_generator_frees));
    iterator_resource_final=(XrXirDomainBudgetStats){0};iterator_resource_previous=(XrXirDomainBudgetStats){0};iterator_resource_disposals=0;
    iterator_index_bounds=0;iterator_index_sticky=0;iterator_index_stale=0;
    IteratorResourceRun run={.fault=limits?XR_XIR_CALL_LIMIT:XR_XIR_CALL_OOM,.complete=true,.pull=1};
    runtime_attempts=0;XrXirProgram *program=iterator_program(&run.fixture,spec->backend);
    IteratorResourceResult result={.setup_sites=runtime_attempts};CHECK(!runtime_live && !runtime_bytes);
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);if(limits) config=*limits;
    config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,iterator_resource_output,&run};runtime_attempts=0;runtime_fail_at=failure;
    XrXirCallStatus status=xr_xir_instance_new(program,&config,&run.instance);
    run.complete=status==XR_XIR_CALL_READY;CHECK(run.complete || (status==run.fault && !run.instance));
    if(run.instance) {
        run.had_domain=true;run.domain=run.instance->domain;iterator_resource_borrowed=run.domain;
        for(unsigned i=0;i<2;++i){run.traces[i].instance=run.instance;run.traces[i].fixture=&run.fixture;run.traces[i].index=i;}
    }
    xr_xir_compile_program_drop(program);CHECK(run.fixture.releases==(run.instance?0u:1u));
    if(run.complete) iterator_index_drive(&run,spec);
    if(run.complete) for(unsigned i=0;i<2;++i) {
        CHECK(run.traces[i].witness.root==(iterator_native(spec->backend,true)?2u:1u));
        CHECK(run.traces[i].witness.producer==(iterator_native(spec->backend,false)?2u:1u));
    }
    iterator_resource_retire(&run);
    IteratorResourceSpec retirement={spec->backend,0,0,spec->victim,1};iterator_resource_late(&run,&retirement);
    CHECK(!iterator_resource_borrowed && iterator_resource_disposals==(run.had_domain?1u:0u));
    result.cost=iterator_resource_final;result.complete=run.complete;result.published=run.had_domain;result.sites=runtime_attempts;
    result.hit=failure!=SIZE_MAX && runtime_attempts>failure;result.cleanup=run.cleanup[0]+run.cleanup[1];result.cells=iterator_cell_freed[0]+iterator_cell_freed[1];
    result.generators=iterator_resource_generator_frees[0]+iterator_resource_generator_frees[1];result.busy=run.busy;result.waits=run.waits;result.phase=run.phase;
    CHECK(result.cost.requested_bytes<=config.requested_value_limit && result.cost.requested_call_bytes<=config.requested_call_limit && result.cost.work<=config.work_limit);
    runtime_fail_at=SIZE_MAX;CHECK(!runtime_live && !runtime_bytes && !effects_compile_live && !effects_compile_bytes);return result;
}
static IteratorResourceResult iterator_index_baseline(const IteratorIndexSpec *spec) {
    IteratorResourceResult r=iterator_index_operation(spec,SIZE_MAX,NULL);
    CHECK(r.complete && r.published && !r.setup_sites && r.sites && r.sites<20000 && r.cleanup==2 && r.cells==2 && r.generators==2 && r.busy==8);
    CHECK(iterator_index_bounds==4 && iterator_index_sticky==6);
    printf("Iterator index resources backend=%u victim=%u stage=%u maximum=%u sites=%zu setupSites=%zu value=%llu call=%llu work=%llu waits=%u bounds=%u sticky=%u stale=%u originalDomain noExtraRetain codeDead rootsDead metadataBeforeStrings domainPhysicalOnce physical0/0\n",spec->backend,spec->victim,spec->stage,spec->maximum,r.sites,r.setup_sites,(unsigned long long)r.cost.requested_bytes,(unsigned long long)r.cost.requested_call_bytes,(unsigned long long)r.cost.work,r.waits,iterator_index_bounds,iterator_index_sticky,iterator_index_stale);
    return r;
}
static void iterator_index_axes(const IteratorIndexSpec *spec,const IteratorResourceResult *normal) {
    const uint64_t values[]={normal->cost.requested_bytes,normal->cost.requested_call_bytes,normal->cost.work};
    for(unsigned axis=0;axis<3;++axis) for(unsigned minus=0;minus<2;++minus) {
        XrXirInstanceConfig config;CHECK(values[axis]>1 && xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
        if(!axis)config.requested_value_limit=values[axis]-minus;else if(axis==1)config.requested_call_limit=values[axis]-minus;else config.work_limit=values[axis]-minus;
        IteratorResourceResult r=iterator_index_operation(spec,SIZE_MAX,&config);CHECK(r.complete==!minus);
        printf("Iterator index axis backend=%u victim=%u stage=%u maximum=%u axis=%u minus=%u complete=%u value=%llu call=%llu work=%llu physical0/0\n",spec->backend,spec->victim,spec->stage,spec->maximum,axis,minus,r.complete,(unsigned long long)r.cost.requested_bytes,(unsigned long long)r.cost.requested_call_bytes,(unsigned long long)r.cost.work);
    }
}
static unsigned iterator_index_number(const char *text) {
    char *end=NULL;unsigned long n=strtoul(text,&end,10);CHECK(text[0] && end && !*end && n<20000);return (unsigned)n;
}
int main(int argc,char **argv) {
    CHECK(argc==3 || argc==9);(void)iterator_graph;(void)iterator_resource_baseline;(void)iterator_resource_axes;
    const char *names[]={"0","1","vm","native"};unsigned backend=0;
    while(backend<4 && strcmp(argv[1],names[backend]))++backend;CHECK(backend<4);
    if(!strcmp(argv[2],"baseline") || !strcmp(argv[2],"axes")) {
        CHECK(argc==3);
        for(unsigned victim=0;victim<2;++victim) for(unsigned stage=0;stage<4;++stage) for(unsigned maximum=0;maximum<2;++maximum) {
            IteratorIndexSpec spec={backend,victim,stage,maximum};IteratorResourceResult normal=iterator_index_baseline(&spec);
            if(!strcmp(argv[2],"axes"))iterator_index_axes(&spec,&normal);
        }
        return 0;
    }
    CHECK(argc==9 && !strcmp(argv[2],"fi"));
    IteratorIndexSpec spec={backend,iterator_index_number(argv[3]),iterator_index_number(argv[4]),iterator_index_number(argv[5])};
    unsigned begin=iterator_index_number(argv[6]),end=iterator_index_number(argv[7]),expected=iterator_index_number(argv[8]);
    IteratorResourceResult normal=iterator_index_baseline(&spec);CHECK(normal.sites==expected && begin<end && end<=expected);
    for(unsigned site=begin;site<end;++site) {
        printf("Iterator index FI start backend=%u victim=%u stage=%u maximum=%u site=%u of%u\n",spec.backend,spec.victim,spec.stage,spec.maximum,site,expected);fflush(stdout);
        IteratorResourceResult r=iterator_index_operation(&spec,site,NULL);CHECK(!r.complete && r.hit);
    }
    printf("Iterator index completeFI backend=%u victim=%u stage=%u maximum=%u range=%u:%u of%u typedOOM originalDomain physical0/0\n",spec.backend,spec.victim,spec.stage,spec.maximum,begin,end,expected);return 0;
}
