/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_mixed_iterator_pull_resources.h - Whole original Domain budgets for cached and indexed pulls
 */
#ifndef XIR_MIXED_ITERATOR_PULL_RESOURCES_H
#define XIR_MIXED_ITERATOR_PULL_RESOURCES_H
#include "xir/xxir_vm.h"
#include "xir/xxir_checked.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if(!(c)){fprintf(stderr,"Iterator resources %d: %s\n",__LINE__,#c);exit(1);} } while(0)
#include "xir_effect_execution_owner.h"
static void iterator_resource_before_free(void *pointer);
#define XIR_RUNTIME_BEFORE_FREE(pointer) iterator_resource_before_free(pointer)
#include "xir_runtime_allocations.h"
#undef XIR_RUNTIME_BEFORE_FREE
#undef CHECK
#define main iterator_lifetime_original_main
#include "test_xir_mixed_iterator_lifetime.c"
#undef main
#undef XIR_RUNTIME_BEFORE_FREE
#undef CHECK
#define CHECK(c) do { if(!(c)){fprintf(stderr,"Iterator resources %d: %s\n",__LINE__,#c);exit(1);} } while(0)

typedef struct IteratorResourceSpec { unsigned backend,first,mode,victim,pull; } IteratorResourceSpec;
typedef struct IteratorResourceResult {
    XrXirDomainBudgetStats cost;
    size_t sites,setup_sites;
    unsigned cleanup,cells,generators,busy,waits,phase,cache_checks,nth_stale;
    bool complete,hit,published;
} IteratorResourceResult;
typedef struct IteratorResourceRun {
    IteratorFixture fixture;
    IteratorTrace traces[2];
    XrXirInstance *instance;
    XrXirDomain *domain;
    uintptr_t addresses[2];
    XrXirInstanceResult first_events[2];
    XrXirTaskWaitToken first_tokens[2];
    XrXirCallStatus fault;
    unsigned cleanup[2],current,operation,busy,waits,phase,pull,cache_checks,nth_stale;
    int64_t index;
    bool complete,driven,had_domain;
} IteratorResourceRun;
static XrXirDomain *iterator_resource_borrowed;
static XrXirDomainBudgetStats iterator_resource_final,iterator_resource_previous;
static unsigned iterator_resource_disposals;
static uintptr_t iterator_resource_generator_owner[2];
static unsigned iterator_resource_generator_frees[2];
static void iterator_resource_monotone(XrXirDomainBudgetStats current) {
    CHECK(current.requested_bytes>=iterator_resource_previous.requested_bytes &&
        current.requested_call_bytes>=iterator_resource_previous.requested_call_bytes && current.work>=iterator_resource_previous.work);
    iterator_resource_previous=current;
}
/* Observe the original allocation without retaining it or replacing its ledger. */
static void iterator_resource_before_free(void *pointer) {
    if(!pointer) return;
    iterator_cell_free(pointer);
    for(unsigned i=0;i<2;++i) if(iterator_resource_generator_owner[i]==(uintptr_t)pointer) {
        iterator_resource_generator_owner[i]=0;++iterator_resource_generator_frees[i];
    }
    if(pointer!=iterator_resource_borrowed) return;
    XrXirDomain *domain=pointer;
    CHECK(!atomic_load_explicit(&domain->references,memory_order_relaxed) && !domain->objects);
    CHECK(domain->stats.live_bytes==sizeof(*domain) && domain->stats.allocations==domain->stats.frees+1);
    CHECK(!domain->budget.call_live && !domain->budget.metadata_live &&
        domain->budget.call_allocations==domain->budget.call_frees && domain->budget.metadata_allocations==domain->budget.metadata_frees);
    iterator_resource_monotone(domain->budget);iterator_resource_final=domain->budget;
    CHECK(!iterator_resource_disposals++);iterator_resource_borrowed=NULL;
}
static void iterator_resource_cost(void) {
    if(iterator_resource_borrowed) iterator_resource_monotone(xr_xir_domain_budget_stats(iterator_resource_borrowed));
}
static bool iterator_resource_status(IteratorResourceRun *run,XrXirCallStatus status,XrXirCallStatus expected) {
    if(status==run->fault){run->complete=false;return false;}
    CHECK(status==expected);return true;
}
static XrXirOutputStatus iterator_resource_output(void *context,const XrXirOutputGroup *group) {
    IteratorResourceRun *run=context;XrXirTaskExecutor *executor=run->instance->executor;
    CHECK(group && group->line && group->stream==XR_XIR_STDOUT && group->count==1 && executor->current);
    CHECK(group->values[0].type==XR_XIR_I64 && group->values[0].payload==701);
    unsigned at=2;
    for(XirGenerator *g=executor->generators;g;g=g->active_next) if(&g->activation==executor->current)
        for(unsigned i=0;i<2;++i) if((uintptr_t)g==run->addresses[i]) at=i;
    CHECK(at<2 && !run->cleanup[at]++);
    XrXirDomainBudgetStats before=xr_xir_domain_budget_stats(run->domain);size_t attempts=runtime_attempts;
    uint64_t epoch=run->instance->epoch;XrXirCall *call=executor->current->call;CHECK(call && call->driving);
    CHECK(xr_xir_instance_poll_bounded(run->instance,1).outcome.status==XR_XIR_CALL_BUSY);
    CHECK(xr_xir_instance_start(run->instance,p20_iterator_lifetime_entries[0],NULL,0)==XR_XIR_CALL_BUSY);
    CHECK(xr_xir_instance_free(run->instance)==XR_XIR_CALL_BUSY && xr_xir_call_free(call)==XR_XIR_CALL_BUSY);
    XrXirDomainBudgetStats after=xr_xir_domain_budget_stats(run->domain);
    CHECK(before.requested_bytes==after.requested_bytes && before.requested_call_bytes==after.requested_call_bytes && before.work==after.work);
    CHECK(attempts==runtime_attempts && epoch==run->instance->epoch);run->busy+=4;return XR_XIR_OUTPUT_OK;
}
static void iterator_resource_observe(IteratorResourceRun *run) {
    CHECK(run->instance && run->instance->domain==run->domain && iterator_resource_borrowed==run->domain);
    iterator_frames(&run->traces[run->current],run->instance->call);
    for(XirGenerator *g=run->instance->executor->generators;g;g=g->active_next) {
        unsigned at=2;
        for(unsigned i=0;i<2;++i) if((uintptr_t)g==run->addresses[i]) at=i;
        if(at==2){CHECK(run->operation==0 && !run->addresses[run->current]);at=run->current;run->addresses[at]=(uintptr_t)g;iterator_resource_generator_owner[at]=(uintptr_t)g;}
        CHECK(g->executor==run->instance->executor && g->object.domain==run->domain);
        iterator_frames(&run->traces[at],g->call);
    }
    if(run->traces[0].witness.cell && run->traces[1].witness.cell) CHECK(run->traces[0].witness.cell!=run->traces[1].witness.cell);
    iterator_resource_cost();
}
static XrXirInstanceResult iterator_resource_tick(IteratorResourceRun *run) {
    XrXirInstanceResult event=xr_xir_instance_poll_bounded(run->instance,1);
    iterator_resource_observe(run);
    if(event.outcome.status==run->fault){run->complete=false;run->driven=true;}
    return event;
}
static bool iterator_resource_start(IteratorResourceRun *run,unsigned at,unsigned operation) {
    CHECK(run->complete && at<2 && operation<4);++run->phase;run->current=at;run->operation=operation;
    XrXirValue arguments[]={run->traces[at].iterator,{XR_XIR_I64,0,run->index}};
    XrXirCallStatus status=xr_xir_instance_start(run->instance,p20_iterator_lifetime_entries[operation],operation?arguments:NULL,operation==3?2u:operation?1u:0u);
    iterator_resource_cost();return iterator_resource_status(run,status,XR_XIR_CALL_READY);
}
static bool iterator_resource_return(IteratorResourceRun *run,XrXirCallStatus expected,XrXirValue *output) {
    unsigned polls=0;XrXirInstanceResult event={0};
    do {event=iterator_resource_tick(run);CHECK(++polls<4096);} while(run->complete && event.outcome.status==XR_XIR_CALL_READY);
    if(!run->complete) return false;
    CHECK(event.outcome.status==expected && xr_xir_call_result_valid(&event.outcome));
    XrXirCallStatus status=xr_xir_instance_take_result(run->instance,output);
    if(expected==XR_XIR_CALL_RETURNED) {
        bool returned=iterator_resource_status(run,status,expected);
        if(!returned) CHECK(!output->type && !output->reserved && !output->payload);return returned;
    }
    CHECK(status==XR_XIR_CALL_BAD_STATE && !output->type && !output->payload);return true;
}
static bool iterator_resource_wait(IteratorResourceRun *run,unsigned milliseconds,XrXirInstanceResult *output) {
    unsigned polls=0;XrXirInstanceResult event={0};
    do {event=iterator_resource_tick(run);CHECK(++polls<4096);} while(run->complete && event.outcome.status==XR_XIR_CALL_READY);
    if(!run->complete) return false;
    CHECK(event.outcome.status==XR_XIR_CALL_SUSPENDED && event.epoch && event.outcome.wake);
    XrXirWaitRequest request={0};
    CHECK(xr_xir_instance_wait_request(run->instance,event.epoch,event.outcome.wake,&request)==XR_XIR_CALL_READY);
    CHECK(request.kind==XR_XIR_WAIT_TIMER_MS && request.after_ms==milliseconds && !request.reserved);
    XirGenerator *g=(XirGenerator *)run->addresses[run->current];CHECK(g && g->call && g->consumer==run->instance->call);
    uint64_t count=g->accounting.polls;XrXirInstanceResult repeated=iterator_resource_tick(run);
    if(!run->complete) return false;
    CHECK(repeated.outcome.status==event.outcome.status && repeated.epoch==event.epoch && repeated.outcome.wake==event.outcome.wake);
    XrXirWaitRequest same={0};CHECK(xr_xir_instance_wait_request(run->instance,repeated.epoch,repeated.outcome.wake,&same)==XR_XIR_CALL_READY);
    CHECK(!memcmp(&same,&request,sizeof(same)) && g->accounting.polls==count);++run->waits;*output=event;return true;
}
static void iterator_pull_remember(IteratorResourceRun *run,unsigned at,XrXirInstanceResult event) {
    run->first_events[at]=event;
    CHECK(xr_xir_call_iterator_wait_token(run->instance->call,&run->first_tokens[at]));
    CHECK(run->first_tokens[at].request.subject==(const void *)run->addresses[at]);
}
static bool iterator_pull_cached_result(IteratorResourceRun *run,unsigned at,unsigned element,bool consume) {
    XrXirValue more={0};
    if(!iterator_resource_return(run,XR_XIR_CALL_RETURNED,&more)) return false;
    CHECK(more.type==XR_XIR_BOOL && more.payload==1);xr_xir_value_drop(&more);
    XirGenerator *g=(XirGenerator *)run->addresses[at];uint64_t polls=g->accounting.polls;
    if(!iterator_resource_start(run,at,2) || !iterator_resource_return(run,XR_XIR_CALL_RETURNED,&more)) return false;
    CHECK(more.type==XR_XIR_BOOL && more.payload==1 && g->accounting.polls==polls);xr_xir_value_drop(&more);++run->cache_checks;
    if(!consume) return true;
    if(!iterator_resource_start(run,at,1) || !iterator_resource_return(run,XR_XIR_CALL_RETURNED,&run->traces[at].held[element])) return false;
    iterator_text(&run->traces[at].held[element],element?"birch":"cedar");
    CHECK(g->accounting.polls==polls && !run->cleanup[at]);++run->cache_checks;return true;
}
static bool iterator_pull_first(IteratorResourceRun *run,unsigned at) {
    XrXirInstanceResult event;run->index=0;
    unsigned operation=run->pull==2?3u:2u;
    if(!iterator_resource_start(run,at,operation) || !iterator_resource_wait(run,5,&event)) return false;
    iterator_pull_remember(run,at,event);
    CHECK(run->first_tokens[at].request.kind==(uint32_t)(run->pull==2?XR_XIR_WAIT_ITERATOR_NTH:XR_XIR_WAIT_ITERATOR_HAS_NEXT));
    CHECK(!run->first_tokens[at].request.after_ms);iterator_resume(&run->traces[at],event);
    if(run->pull!=2) return iterator_pull_cached_result(run,at,0,run->pull==1);
    if(!iterator_resource_return(run,XR_XIR_CALL_RETURNED,&run->traces[at].held[0])) return false;
    iterator_text(&run->traces[at].held[0],"cedar");return true;
}
/* The uncached indexed pull owns one real request across both Timer waits. */
static bool iterator_pull_demand(IteratorResourceRun *run,unsigned at,bool uncached,XrXirInstanceResult *event) {
    run->index=run->pull>=3?1:0;unsigned operation=run->pull==1?2u:3u;
    if(!iterator_resource_start(run,at,operation)) return false;
    XrXirTaskWaitToken before={0};XrXirInstanceResult first={0};
    if(uncached) {
        CHECK(run->pull==4 && operation==3);
        if(!iterator_resource_wait(run,5,&first)) return false;
        iterator_pull_remember(run,at,first);before=run->first_tokens[at];
        CHECK(before.request.kind==XR_XIR_WAIT_ITERATOR_NTH && before.request.after_ms==1);
        iterator_resume(&run->traces[at],first);
    }
    if(!iterator_resource_wait(run,7,event)) return false;
    if(operation==3) {
        XrXirTaskWaitToken current={0};CHECK(xr_xir_call_iterator_wait_token(run->instance->call,&current));
        CHECK(current.request.kind==XR_XIR_WAIT_ITERATOR_NTH && !current.request.after_ms && current.request.subject==(const void *)run->addresses[at]);
    }
    if(uncached) {
        CHECK(event->epoch==first.epoch && event->outcome.wake!=first.outcome.wake);
        CHECK(xr_xir_instance_resume(run->instance,first.epoch,first.outcome.wake)==XR_XIR_CALL_BAD_STATE);
        CHECK(xr_xir_call_complete_iterator_wait(run->instance->call,&before)==XR_XIR_CALL_BAD_STATE);++run->nth_stale;
    }
    return true;
}
static bool iterator_pull_complete(IteratorResourceRun *run,unsigned at) {
    if(run->pull==1) return iterator_pull_cached_result(run,at,1,true);
    if(!iterator_resource_return(run,XR_XIR_CALL_RETURNED,&run->traces[at].held[1])) return false;
    iterator_text(&run->traces[at].held[1],"birch");CHECK(!run->cleanup[at]);return true;
}
static bool iterator_pull_second(IteratorResourceRun *run,unsigned at,bool uncached) {
    XrXirInstanceResult event;
    if(!iterator_pull_demand(run,at,uncached,&event)) return false;
    iterator_resume(&run->traces[at],event);return iterator_pull_complete(run,at);
}

static bool iterator_resource_end(IteratorResourceRun *run,unsigned at) {
    XrXirValue value={0};
    if(!iterator_resource_start(run,at,2) || !iterator_resource_return(run,XR_XIR_CALL_RETURNED,&value)) return false;
    CHECK(value.type==XR_XIR_BOOL && !value.payload && run->cleanup[at]==1);xr_xir_value_drop(&value);
    XirGenerator *g=(XirGenerator *)run->addresses[at];uint64_t polls=g->accounting.polls;
    CHECK(!g->call && !g->consumer && g->outcome.status==XR_XIR_CALL_RETURNED);
    for(unsigned i=0;i<2;++i) {
        if(!iterator_resource_start(run,at,1)) return false;
        unsigned steps=0;XrXirInstanceResult event={0};
        do {event=iterator_resource_tick(run);CHECK(++steps<4096);} while(run->complete && event.outcome.status==XR_XIR_CALL_READY);
        if(!run->complete) return false;
        CHECK(event.outcome.status==XR_XIR_CALL_RUNTIME_PANIC && event.outcome.panic.detail.code==XR_XIR_PANIC_ITERATOR_EXHAUSTED);
        CHECK(xr_xir_call_result_valid(&event.outcome) && g->accounting.polls==polls && run->cleanup[at]==1);
        CHECK(xr_xir_instance_take_result(run->instance,&value)==XR_XIR_CALL_BAD_STATE && !value.type && !value.payload);
    }
    return true;
}
static bool iterator_resource_make(IteratorResourceRun *run) {
    for(unsigned i=0;i<2;++i) {
        if(!iterator_resource_start(run,i,0) || !iterator_resource_return(run,XR_XIR_CALL_RETURNED,&run->traces[i].iterator)) return false;
        CHECK(run->addresses[i]==(uintptr_t)run->traces[i].iterator.payload && xr_xir_value_valid(&run->traces[i].iterator));
        XrXirValueStatus copied=xr_xir_value_copy(&run->traces[i].iterator,&run->traces[i].alias);
        if(copied!=XR_XIR_VALUE_OK){CHECK(copied==(run->fault==XR_XIR_CALL_LIMIT?XR_XIR_VALUE_LIMIT:XR_XIR_VALUE_OOM));CHECK(!run->traces[i].alias.type && !run->traces[i].alias.payload);run->complete=false;return false;}
    }
    XirGenerator *a=(XirGenerator *)run->addresses[0],*b=(XirGenerator *)run->addresses[1];
    CHECK(a!=b && a->identity!=b->identity && a->generation==b->generation && a->call!=b->call && a->object.domain==b->object.domain);
    return true;
}
static bool iterator_resource_orphan(IteratorResourceRun *run,unsigned victim,XrXirInstanceResult event) {
    unsigned healthy=1-victim;XirGenerator *peer=(XirGenerator *)run->addresses[healthy];
    XrXirCall *call=peer->call;uint64_t polls=peer->accounting.polls;
    xr_xir_value_drop(&run->traces[victim].iterator);CHECK(iterator_physical(run->addresses[victim]));
    xr_xir_value_drop(&run->traces[victim].alias);size_t attempts=runtime_attempts;
    xr_xir_value_drop(&run->traces[victim].iterator);xr_xir_value_drop(&run->traces[victim].alias);CHECK(attempts==runtime_attempts);
    unsigned steps=0;
    while(iterator_resource_generator_owner[victim]) {
        XrXirInstanceResult result=iterator_resource_tick(run);CHECK(++steps<4096);
        if(!run->complete) return false;
        CHECK(result.outcome.status==XR_XIR_CALL_READY || result.outcome.status==XR_XIR_CALL_SUSPENDED);
    }
    CHECK(run->cleanup[victim]==1 && iterator_cell_freed[victim]==1 && !run->cleanup[healthy] && iterator_resource_generator_frees[victim]==1);
    CHECK(peer->call==call && peer->accounting.polls==polls && peer->consumer==run->instance->call);
    XrXirInstanceResult repeated=iterator_resource_tick(run);if(!run->complete) return false;
    CHECK(repeated.outcome.status==XR_XIR_CALL_SUSPENDED && repeated.epoch==event.epoch && repeated.outcome.wake==event.outcome.wake);
    return true;
}
static bool iterator_resource_control(IteratorResourceRun *run,const IteratorResourceSpec *spec) {
    unsigned at=1-spec->victim;XrXirInstanceResult event;
    if(!iterator_pull_demand(run,at,run->pull==4,&event)) return false;
    for(unsigned i=0;i<2;++i) {
        CHECK(xr_xir_instance_resume(run->instance,run->first_events[i].epoch,run->first_events[i].outcome.wake)==XR_XIR_CALL_BAD_STATE);
        CHECK(xr_xir_call_complete_iterator_wait(run->instance->call,&run->first_tokens[i])==XR_XIR_CALL_BAD_STATE);
    }
    if(spec->mode==1) {
        if(!iterator_resource_orphan(run,spec->victim,event)) return false;
        iterator_resume(&run->traces[at],event);
        return iterator_pull_complete(run,at) && iterator_resource_end(run,at);
    }
    if(spec->mode==2) {
        CHECK(xr_xir_instance_cancel_current(run->instance)==XR_XIR_CALL_CANCEL_REQUESTED);
        CHECK(xr_xir_instance_resume(run->instance,event.epoch,event.outcome.wake)==XR_XIR_CALL_BAD_STATE);
        XrXirValue empty={0};if(!iterator_resource_return(run,XR_XIR_CALL_CANCELLED,&empty)) return false;
        XirGenerator *g=(XirGenerator *)run->addresses[at];CHECK(g->outcome.status==XR_XIR_CALL_CANCELLED && run->cleanup[at]==1);
        return iterator_pull_second(run,spec->victim,false) && iterator_resource_end(run,spec->victim);
    }
    XrXirCallStatus status=spec->mode==3?xr_xir_instance_stop(run->instance):xr_xir_instance_free(run->instance);
    if(spec->mode==4) run->instance=NULL;
    if(!iterator_resource_status(run,status,XR_XIR_CALL_READY)) return false;
    CHECK(run->cleanup[0]==1 && run->cleanup[1]==1);return true;
}

static void iterator_resource_drive(IteratorResourceRun *run,const IteratorResourceSpec *spec) {
    if(!iterator_resource_make(run)) return;
    if(run->pull==4) {
        if(!iterator_pull_first(run,spec->victim)) return;
    } else for(unsigned n=0;n<2;++n) {
        unsigned at=n?1-spec->first:spec->first,peer=1-at;
        XirGenerator *g=(XirGenerator *)run->addresses[peer];uint64_t polls=g->accounting.polls;
        if(!iterator_pull_first(run,at)) return;
        CHECK(g->accounting.polls==polls);
    }
    if(spec->mode){(void)iterator_resource_control(run,spec);return;}
    if(run->pull==4) {
        if(!iterator_pull_second(run,1-spec->victim,true) || !iterator_pull_second(run,spec->victim,false)) return;
    } else for(unsigned n=0;n<2;++n) {
        unsigned at=n?1-spec->first:spec->first;
        if(!iterator_pull_second(run,at,false)) return;
    }
    for(unsigned i=0;i<2;++i) if(!iterator_resource_end(run,i)) return;
}

static void iterator_resource_retire(IteratorResourceRun *run) {
    if(!run->instance) return;
    bool failed=run->driven || run->instance->budget.exhausted;
    XrXirCallStatus status=xr_xir_instance_free(run->instance);run->instance=NULL;
    if(failed) CHECK(status==run->fault);
    else CHECK(status==XR_XIR_CALL_READY || status==run->fault);
    if(status==run->fault) run->complete=false;
}
static void iterator_resource_late(IteratorResourceRun *run,const IteratorResourceSpec *spec) {
    CHECK(run->fixture.releases==1 && !run->fixture.vm && !run->fixture.entries);
    for(unsigned i=0;i<2;++i) {
        if(run->traces[i].iterator.type) {
            const XirGenerator *g=(const XirGenerator *)(uintptr_t)run->traces[i].iterator.payload;
            CHECK(xr_xir_value_valid(&run->traces[i].iterator) && !g->executor && !g->call && !g->consumer);
            if(run->complete) CHECK(g->outcome.status==((spec->mode>=3 || (spec->mode==2 && i!=spec->victim))?XR_XIR_CALL_CANCELLED:XR_XIR_CALL_RETURNED));
            else CHECK(g->outcome.status==XR_XIR_CALL_RETURNED || g->outcome.status==XR_XIR_CALL_CANCELLED || g->outcome.status==run->fault);
        }
        size_t attempts=runtime_attempts;xr_xir_value_drop(&run->traces[i].iterator);
        if(run->traces[i].alias.type) CHECK(iterator_resource_generator_owner[i]==run->addresses[i] && iterator_physical(run->addresses[i]));
        xr_xir_value_drop(&run->traces[i].alias);CHECK(runtime_attempts==attempts);
        if(run->addresses[i]) CHECK(!iterator_resource_generator_owner[i] && iterator_resource_generator_frees[i]==1);
        if(run->traces[i].witness.cell) CHECK(iterator_cell_freed[i]==1 && !iterator_cell_owner[i]);
    }
    CHECK(!effects_compile_live && !effects_compile_bytes);
    for(unsigned i=0;i<2;++i) for(unsigned j=0;j<2;++j) if(run->traces[i].held[j].type) {
        iterator_text(&run->traces[i].held[j],j?"birch":"cedar");xr_xir_value_drop(&run->traces[i].held[j]);
    }
}
static IteratorResourceResult iterator_resource_operation(const IteratorResourceSpec *spec,size_t failure,const XrXirInstanceConfig *limits) {
    CHECK(spec->backend<4 && spec->first<2 && spec->mode<5 && spec->victim<2 && spec->pull>=1 && spec->pull<=4);
    CHECK(spec->pull==4?!spec->first:(spec->mode || !spec->victim));
    CHECK(!iterator_resource_borrowed && !runtime_live && !effects_compile_live);runtime_fail_at=SIZE_MAX;
    memset(iterator_cell_owner,0,sizeof(iterator_cell_owner));memset(iterator_cell_freed,0,sizeof(iterator_cell_freed));
    memset(iterator_resource_generator_owner,0,sizeof(iterator_resource_generator_owner));memset(iterator_resource_generator_frees,0,sizeof(iterator_resource_generator_frees));
    iterator_resource_final=(XrXirDomainBudgetStats){0};iterator_resource_previous=(XrXirDomainBudgetStats){0};iterator_resource_disposals=0;
    IteratorResourceRun run={.fault=limits?XR_XIR_CALL_LIMIT:XR_XIR_CALL_OOM,.complete=true,.pull=spec->pull};
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
    if(run.complete) iterator_resource_drive(&run,spec);
    if(run.complete) {
        for(unsigned i=0;i<2;++i) {
            CHECK(run.traces[i].witness.root==(iterator_native(spec->backend,true)?2u:1u));
            CHECK(run.traces[i].witness.producer==(iterator_native(spec->backend,false)?2u:1u));
        }
    }
    iterator_resource_retire(&run);iterator_resource_late(&run,spec);
    CHECK(!iterator_resource_borrowed && iterator_resource_disposals==(run.had_domain?1u:0u));
    result.cost=iterator_resource_final;result.complete=run.complete;result.published=run.had_domain;result.sites=runtime_attempts;
    result.hit=failure!=SIZE_MAX && runtime_attempts>failure;result.cleanup=run.cleanup[0]+run.cleanup[1];result.cells=iterator_cell_freed[0]+iterator_cell_freed[1];
    result.generators=iterator_resource_generator_frees[0]+iterator_resource_generator_frees[1];result.cache_checks=run.cache_checks;result.nth_stale=run.nth_stale;result.busy=run.busy;result.waits=run.waits;result.phase=run.phase;
    CHECK(result.cost.requested_bytes<=config.requested_value_limit && result.cost.requested_call_bytes<=config.requested_call_limit && result.cost.work<=config.work_limit);
    runtime_fail_at=SIZE_MAX;CHECK(!runtime_live && !runtime_bytes && !effects_compile_live && !effects_compile_bytes);return result;
}
static IteratorResourceResult iterator_resource_baseline(const IteratorResourceSpec *spec) {
    IteratorResourceResult r=iterator_resource_operation(spec,SIZE_MAX,NULL);
    CHECK(r.complete && r.published && !r.setup_sites && r.sites && r.sites<20000 && r.cleanup==2 && r.cells==2 && r.generators==2 && r.busy==8);
    printf("Iterator pull resources backend=%u pull=%u first=%u mode=%u victim=%u sites=%zu setupSites=%zu value=%llu call=%llu work=%llu waits=%u cache=%u nthStale=%u originalDomain noExtraRetain codeDead rootsDead metadataBeforeStrings domainPhysicalOnce physical0/0\n",spec->backend,spec->pull,spec->first,spec->mode,spec->victim,r.sites,r.setup_sites,(unsigned long long)r.cost.requested_bytes,(unsigned long long)r.cost.requested_call_bytes,(unsigned long long)r.cost.work,r.waits,r.cache_checks,r.nth_stale);
    return r;
}
static void iterator_resource_axes(const IteratorResourceSpec *spec,const IteratorResourceResult *normal) {
    const uint64_t values[]={normal->cost.requested_bytes,normal->cost.requested_call_bytes,normal->cost.work};
    for(unsigned axis=0;axis<3;++axis) for(unsigned minus=0;minus<2;++minus) {
        XrXirInstanceConfig config;CHECK(values[axis]>1 && xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
        if(!axis)config.requested_value_limit=values[axis]-minus;else if(axis==1)config.requested_call_limit=values[axis]-minus;else config.work_limit=values[axis]-minus;
        IteratorResourceResult r=iterator_resource_operation(spec,SIZE_MAX,&config);
        if(r.complete!=!minus)fprintf(stderr,"Iterator pull axis backend=%u pull=%u first=%u mode=%u victim=%u axis=%u minus=%u complete=%u phase=%u\n",spec->backend,spec->pull,spec->first,spec->mode,spec->victim,axis,minus,r.complete,r.phase);
        CHECK(r.complete==!minus);
        printf("Iterator pull axis backend=%u pull=%u first=%u mode=%u victim=%u axis=%u minus=%u complete=%u value=%llu call=%llu work=%llu physical0/0\n",spec->backend,spec->pull,spec->first,spec->mode,spec->victim,axis,minus,r.complete,(unsigned long long)r.cost.requested_bytes,(unsigned long long)r.cost.requested_call_bytes,(unsigned long long)r.cost.work);
    }
}

#endif // XIR_MIXED_ITERATOR_PULL_RESOURCES_H
