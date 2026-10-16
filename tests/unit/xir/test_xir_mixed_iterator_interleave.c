/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_mixed_iterator_interleave.c - Two authentic producers share one executor without sharing state
 */
#define main iterator_lifetime_original_main
#include "test_xir_mixed_iterator_lifetime.c"
#undef main

typedef struct IteratorPair {
    IteratorFixture fixture;
    IteratorTrace traces[2];
    XrXirInstance *instance;
    uintptr_t addresses[2];
    unsigned cleanup[2], current, operation, busy;
} IteratorPair;

static XrXirOutputStatus pair_output(void *context,const XrXirOutputGroup *group) {
    IteratorPair *pair=context;XrXirTaskExecutor *executor=pair->instance->executor;
    CHECK(group && group->line && group->stream==XR_XIR_STDOUT && group->count==1);
    CHECK(group->values[0].type==XR_XIR_I64 && group->values[0].payload==701 && executor->current);
    unsigned at=2;
    for(XirGenerator *g=executor->generators;g;g=g->active_next)
        if(&g->activation==executor->current)
            for(unsigned i=0;i<2;++i) if((uintptr_t)g==pair->addresses[i]) at=i;
    CHECK(at<2 && !pair->cleanup[at]++);
    XrXirCall *call=executor->current->call;CHECK(call && call->driving);
    uint64_t epoch=pair->instance->epoch;size_t attempts=runtime_attempts;
    XrXirDomainBudgetStats before=xr_xir_domain_budget_stats(pair->instance->domain);
    CHECK(xr_xir_instance_poll_bounded(pair->instance,1).outcome.status==XR_XIR_CALL_BUSY);
    CHECK(xr_xir_instance_start(pair->instance,p20_iterator_lifetime_entries[0],NULL,0)==XR_XIR_CALL_BUSY);
    CHECK(xr_xir_instance_free(pair->instance)==XR_XIR_CALL_BUSY);
    CHECK(xr_xir_call_free(call)==XR_XIR_CALL_BUSY);pair->busy+=4;
    XrXirDomainBudgetStats after=xr_xir_domain_budget_stats(pair->instance->domain);
    CHECK(pair->instance->epoch==epoch && runtime_attempts==attempts);
    CHECK(before.requested_bytes==after.requested_bytes && before.requested_call_bytes==after.requested_call_bytes && before.work==after.work);
    return XR_XIR_OUTPUT_OK;
}
static XrXirInstanceResult pair_tick(IteratorPair *pair) {
    XrXirInstanceResult result=xr_xir_instance_poll_bounded(pair->instance,1);
    iterator_frames(&pair->traces[pair->current],pair->instance->call);
    for(XirGenerator *g=pair->instance->executor->generators;g;g=g->active_next) {
        unsigned at=2;
        for(unsigned i=0;i<2;++i) if((uintptr_t)g==pair->addresses[i]) at=i;
        if(at==2) {
            CHECK(pair->operation==0 && !pair->addresses[pair->current] && !pair->traces[pair->current].iterator.type);
            at=pair->current;pair->addresses[at]=(uintptr_t)g;
        }
        CHECK(at<2 && g->executor==pair->instance->executor && g->object.domain==pair->instance->domain);
        iterator_frames(&pair->traces[at],g->call);
    }
    return result;
}
static XrXirValue pair_return(IteratorPair *pair,XrXirCallStatus expected) {
    XrXirInstanceResult result={0};unsigned polls=0;
    do {result=pair_tick(pair);CHECK(++polls<4096);} while(result.outcome.status==XR_XIR_CALL_READY);
    CHECK(result.outcome.status==expected);XrXirValue value={0};
    CHECK(xr_xir_instance_take_result(pair->instance,&value)==
        (expected==XR_XIR_CALL_RETURNED ? expected : XR_XIR_CALL_BAD_STATE));
    return value;
}
static void pair_start(IteratorPair *pair,unsigned at,unsigned operation) {
    pair->current=at;pair->operation=operation;iterator_start(&pair->traces[at],operation,0);
}
static XrXirInstanceResult pair_wait(IteratorPair *pair,unsigned milliseconds) {
    XrXirInstanceResult result={0};unsigned polls=0;
    do {result=pair_tick(pair);CHECK(++polls<4096);} while(result.outcome.status==XR_XIR_CALL_READY);
    CHECK(result.outcome.status==XR_XIR_CALL_SUSPENDED && result.epoch && result.outcome.wake);
    XrXirWaitRequest request={0};
    CHECK(xr_xir_instance_wait_request(pair->instance,result.epoch,result.outcome.wake,&request)==XR_XIR_CALL_READY);
    CHECK(request.kind==XR_XIR_WAIT_TIMER_MS && request.after_ms==milliseconds && !request.reserved);
    XirGenerator *g=(XirGenerator *)(uintptr_t)pair->traces[pair->current].iterator.payload;
    CHECK(g->consumer==pair->instance->call && g->call);
    uint64_t polls_before=g->accounting.polls;
    XrXirInstanceResult same=pair_tick(pair);XrXirWaitRequest repeated={0};
    CHECK(same.outcome.status==result.outcome.status && same.epoch==result.epoch && same.outcome.wake==result.outcome.wake);
    CHECK(xr_xir_instance_wait_request(pair->instance,same.epoch,same.outcome.wake,&repeated)==XR_XIR_CALL_READY);
    CHECK(!memcmp(&request,&repeated,sizeof(request)) && g->accounting.polls==polls_before);
    return result;
}
static void pair_make(IteratorPair *pair,unsigned backend) {
    memset(iterator_cell_owner,0,sizeof(iterator_cell_owner));memset(iterator_cell_freed,0,sizeof(iterator_cell_freed));
    XrXirProgram *program=iterator_program(&pair->fixture,backend);
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,pair_output,pair};
    CHECK(xr_xir_instance_new(program,&config,&pair->instance)==XR_XIR_CALL_READY);
    xr_xir_compile_program_drop(program);CHECK(!pair->fixture.releases);
    for(unsigned i=0;i<2;++i) {
        pair->traces[i].fixture=&pair->fixture;pair->traces[i].index=i;pair->traces[i].instance=pair->instance;
        pair_start(pair,i,0);pair->traces[i].iterator=pair_return(pair,XR_XIR_CALL_RETURNED);
        CHECK(xr_xir_value_copy(&pair->traces[i].iterator,&pair->traces[i].alias)==XR_XIR_VALUE_OK);
        CHECK(pair->addresses[i]==(uintptr_t)pair->traces[i].iterator.payload && iterator_physical(pair->addresses[i]));
    }
    XirGenerator *a=(XirGenerator *)pair->addresses[0],*b=(XirGenerator *)pair->addresses[1];
    CHECK(a!=b && a->call!=b->call && a->generation==b->generation && a->identity!=b->identity);
    CHECK(a->executor==b->executor && a->object.domain==b->object.domain && a->object.domain==pair->instance->domain);
}
static void pair_first_elements(IteratorPair *pair,unsigned first,XrXirInstanceResult events[2]) {
    for(unsigned i=0;i<2;++i) {
        unsigned at=i ? 1-first : first;unsigned peer=1-at;
        XirGenerator *other=(XirGenerator *)pair->addresses[peer];uint64_t peer_polls=other->accounting.polls;
        pair_start(pair,at,1);events[at]=pair_wait(pair,5);iterator_resume(&pair->traces[at],events[at]);
        pair->traces[at].held[0]=pair_return(pair,XR_XIR_CALL_RETURNED);
        iterator_text(&pair->traces[at].held[0],"cedar");CHECK(!pair->cleanup[at]);
        CHECK(other->accounting.polls==peer_polls);
    }
    CHECK(pair->traces[0].witness.cell && pair->traces[1].witness.cell &&
        pair->traces[0].witness.cell!=pair->traces[1].witness.cell);
}
static void pair_orphan(IteratorPair *pair,unsigned victim,XrXirInstanceResult event) {
    unsigned healthy=1-victim;XirGenerator *peer=(XirGenerator *)pair->addresses[healthy];
    XrXirCall *peer_call=peer->call;uint64_t peer_polls=peer->accounting.polls;
    xr_xir_value_drop(&pair->traces[victim].iterator);CHECK(iterator_physical(pair->addresses[victim]));
    xr_xir_value_drop(&pair->traces[victim].alias);
    CHECK(!pair->traces[victim].iterator.type && !pair->traces[victim].alias.type);
    CHECK(iterator_physical(pair->addresses[victim]));
    size_t attempts=runtime_attempts;
    xr_xir_value_drop(&pair->traces[victim].iterator);xr_xir_value_drop(&pair->traces[victim].alias);
    CHECK(runtime_attempts==attempts);
    unsigned polls=0;
    while(iterator_physical(pair->addresses[victim])) {
        XrXirInstanceResult result=pair_tick(pair);CHECK(++polls<4096);
        CHECK(result.outcome.status==XR_XIR_CALL_READY || result.outcome.status==XR_XIR_CALL_SUSPENDED);
    }
    CHECK(pair->cleanup[victim]==1 && iterator_cell_freed[victim]==1 && !iterator_cell_owner[victim]);
    CHECK(!pair->cleanup[healthy] && peer->call==peer_call && peer->accounting.polls==peer_polls && peer->consumer==pair->instance->call);
    XrXirInstanceResult repeated=pair_tick(pair);XrXirWaitRequest request={0};
    CHECK(repeated.outcome.status==XR_XIR_CALL_SUSPENDED && repeated.epoch==event.epoch && repeated.outcome.wake==event.outcome.wake);
    CHECK(xr_xir_instance_wait_request(pair->instance,event.epoch,event.outcome.wake,&request)==XR_XIR_CALL_READY);
    CHECK(request.kind==XR_XIR_WAIT_TIMER_MS && request.after_ms==7);
}
static void pair_exhausted(IteratorPair *pair,unsigned healthy) {
    pair_start(pair,healthy,2);XrXirValue more=pair_return(pair,XR_XIR_CALL_RETURNED);
    CHECK(more.type==XR_XIR_BOOL && !more.payload && pair->cleanup[healthy]==1);xr_xir_value_drop(&more);
    XirGenerator *g=(XirGenerator *)pair->addresses[healthy];uint64_t polls_before=g->accounting.polls;
    for(unsigned i=0;i<2;++i) {
        pair_start(pair,healthy,1);XrXirInstanceResult result={0};unsigned polls=0;
        do {result=pair_tick(pair);CHECK(++polls<4096);} while(result.outcome.status==XR_XIR_CALL_READY);
        CHECK(result.outcome.status==XR_XIR_CALL_RUNTIME_PANIC && result.outcome.panic.detail.code==XR_XIR_PANIC_ITERATOR_EXHAUSTED);
        CHECK(xr_xir_call_result_valid(&result.outcome) && g->accounting.polls==polls_before && pair->cleanup[healthy]==1);
        XrXirValue empty={0};CHECK(xr_xir_instance_take_result(pair->instance,&empty)==XR_XIR_CALL_BAD_STATE && !empty.type && !empty.payload);
    }
}
static void pair_drop(IteratorPair *pair,unsigned healthy) {
    CHECK(xr_xir_instance_free(pair->instance)==XR_XIR_CALL_READY);pair->instance=NULL;
    CHECK(pair->fixture.releases==1 && !pair->fixture.vm && !pair->fixture.entries);
    XirGenerator *g=(XirGenerator *)pair->addresses[healthy];
    CHECK(!g->executor && !g->call && !g->consumer && g->outcome.status==XR_XIR_CALL_RETURNED);
    size_t attempts=runtime_attempts;
    xr_xir_value_drop(&pair->traces[healthy].iterator);CHECK(iterator_physical(pair->addresses[healthy]));
    xr_xir_value_drop(&pair->traces[healthy].alias);CHECK(!iterator_physical(pair->addresses[healthy]));
    CHECK(iterator_cell_freed[healthy]==1 && !iterator_cell_owner[healthy] && runtime_attempts==attempts);
    CHECK(!effects_compile_live && !effects_compile_bytes);
    for(unsigned i=0;i<2;++i) for(unsigned j=0;j<2;++j) if(pair->traces[i].held[j].type) {
        iterator_text(&pair->traces[i].held[j],j ? "birch" : "cedar");xr_xir_value_drop(&pair->traces[i].held[j]);
    }
    CHECK(!runtime_live && !runtime_bytes && pair->busy==8);
}
static void pair_graph(unsigned backend,unsigned first,unsigned victim) {
    IteratorPair pair={0};pair_make(&pair,backend);XrXirInstanceResult events[2];
    pair_first_elements(&pair,first,events);unsigned healthy=1-victim;
    pair_start(&pair,healthy,1);XrXirInstanceResult event=pair_wait(&pair,7);
    pair_orphan(&pair,victim,event);
    for(unsigned i=0;i<2;++i)
        CHECK(xr_xir_instance_resume(pair.instance,events[i].epoch,events[i].outcome.wake)==XR_XIR_CALL_BAD_STATE);
    iterator_resume(&pair.traces[healthy],event);pair.traces[healthy].held[1]=pair_return(&pair,XR_XIR_CALL_RETURNED);
    iterator_text(&pair.traces[healthy].held[1],"birch");pair_exhausted(&pair,healthy);
    for(unsigned i=0;i<2;++i) {
        CHECK(pair.traces[i].witness.root==(iterator_native(backend,true) ? 2u : 1u));
        CHECK(pair.traces[i].witness.producer==(iterator_native(backend,false) ? 2u : 1u));
    }
    pair_drop(&pair,healthy);
    printf("P20 Iterator pair backend=%u first=%u victim=%u sameInstance sameDomain alternatingNext3 orphanWhilePeerWait7 cleanup2 cellFree2 BUSY8 exhaustion2 codeLease1 lateStrings3 physical0/0\n",backend,first,victim);
}
int main(int argc,char **argv) {
    CHECK(argc==2);const char *names[]={"0","1","vm","native"};unsigned backend=0;
    while(backend<4 && strcmp(argv[1],names[backend]))++backend;CHECK(backend<4);
    for(unsigned first=0;first<2;++first) for(unsigned victim=0;victim<2;++victim)pair_graph(backend,first,victim);
    return 0;
}
