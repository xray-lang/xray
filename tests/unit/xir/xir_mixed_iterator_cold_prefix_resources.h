/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_mixed_iterator_cold_prefix_resources.h - Actual defer registration governs cold pull cleanup
 */
#ifndef XIR_MIXED_ITERATOR_COLD_PREFIX_RESOURCES_H
#define XIR_MIXED_ITERATOR_COLD_PREFIX_RESOURCES_H
#include "xir_mixed_iterator_prefix_resources.h"
typedef struct IteratorColdSpec { unsigned backend,kind,peer,victim,mode,prefix; } IteratorColdSpec;
typedef struct IteratorColdRun {
    IteratorPrefixBudgetRun hot;
    unsigned registered[2],entered[2],before_registered[2],before_entered[2],expected_cleanup[2],timers;
} IteratorColdRun;
typedef struct IteratorColdResult {
    IteratorResourceResult resource;
    IteratorPrefixResult progress;
    unsigned registered,entered,peer_registered,policy;
} IteratorColdResult;

/* Reading a stopped real frame does not register cleanup or retain its Cell. */
static void iterator_cold_observe(IteratorColdRun *run) {
    iterator_prefix_budget_capture(&run->hot);IteratorResourceRun *r=&run->hot.base;
    for(unsigned i=0;i<2;++i) {
        if(!r->addresses[i] || iterator_resource_generator_owner[i]!=r->addresses[i])continue;
        XirGenerator *g=(XirGenerator *)r->addresses[i];
        if(!g->call)continue;CHECK(!g->call->driving);
        for(const CallFrame *frame=g->call->top;frame;frame=frame->parent) {
            if(!(frame->entry->flags&XR_XIR_ENTRY_GENERATOR))continue;
            CHECK(frame->entry->flags&XR_XIR_ENTRY_EXIT);
            if(frame->entered)run->entered[i]=1;
            if(frame->cleanup_frontier){CHECK(frame->entered);run->registered[i]=1;}
        }
    }
}
static XrXirInstanceResult iterator_cold_tick(IteratorColdRun *run) {
    XrXirInstanceResult event=iterator_resource_tick(&run->hot.base);iterator_cold_observe(run);return event;
}
/* A producer can publish its fault one bounded poll before its consumer observes it. */
static bool iterator_cold_fault_to_root(IteratorColdRun *run,XrXirInstanceResult *event) {
    IteratorPrefixBudgetRun *h=&run->hot;IteratorResourceRun *r=&h->base;bool failed=false;
    for(unsigned i=0;i<2;++i)if(h->seen[i] && h->sticky[i]==r->fault)failed=true;
    if(!failed)return false;unsigned polls=0;
    while(event->outcome.status==XR_XIR_CALL_READY){CHECK(++polls<4096);*event=iterator_cold_tick(run);++h->progress.polls;}
    CHECK(event->outcome.status==r->fault && xr_xir_call_result_valid(&event->outcome) && !r->complete && r->driven);
    printf("Iterator cold producer fault root=%u producer=%u propagationPolls=%u typedBeforeControl physicalCheckedAtRetirement\n",event->outcome.status,r->fault,polls);return true;
}
static unsigned iterator_cold_entry(unsigned kind){CHECK(kind<7);return kind==0?1u:kind==1?2u:3u;}
static int64_t iterator_cold_index(unsigned kind){return kind==3?1:kind==4?2:kind==5?-1:kind==6?INT64_MIN:0;}
static bool iterator_cold_step(IteratorColdRun *run,const IteratorColdSpec *spec,XrXirInstanceResult *event) {
    IteratorResourceRun *r=&run->hot.base;
    if(event->outcome.status==XR_XIR_CALL_SUSPENDED)iterator_resume(&r->traces[spec->victim],*event);
    *event=iterator_cold_tick(run);++run->hot.progress.polls;if(!r->complete)return false;
    if(iterator_cold_fault_to_root(run,event))return false;
    if(event->outcome.status==XR_XIR_CALL_SUSPENDED) {
        XrXirWaitRequest request={0};CHECK(xr_xir_instance_wait_request(r->instance,event->epoch,event->outcome.wake,&request)==XR_XIR_CALL_READY);
        CHECK(spec->kind<5 && request.kind==XR_XIR_WAIT_TIMER_MS && !request.reserved);
        CHECK(request.after_ms==(run->timers?7u:5u) && run->timers<(spec->kind>=3?2u:1u));++run->timers;++r->waits;
        XrXirTaskWaitToken token={0};CHECK(xr_xir_call_iterator_wait_token(r->instance->call,&token));
        CHECK(token.request.subject==(const void *)r->addresses[spec->victim]);
        uint32_t kind=spec->kind==0?XR_XIR_WAIT_ITERATOR_NEXT:spec->kind==1?XR_XIR_WAIT_ITERATOR_HAS_NEXT:XR_XIR_WAIT_ITERATOR_NTH;
        CHECK(token.request.kind==kind && token.request.after_ms==(uint64_t)(spec->kind>=2?iterator_cold_index(spec->kind)-(int64_t)(run->timers-1):0));
    } else CHECK(event->outcome.status==XR_XIR_CALL_READY || event->outcome.status==XR_XIR_CALL_RETURNED || event->outcome.status==XR_XIR_CALL_RUNTIME_PANIC || event->outcome.status==XR_XIR_CALL_BOUNDS);
    return true;
}
static bool iterator_cold_returned(IteratorColdRun *run,const IteratorColdSpec *spec,XrXirInstanceResult event) {
    CHECK(xr_xir_call_result_valid(&event.outcome));IteratorResourceRun *r=&run->hot.base;
    if(spec->kind>=5){CHECK(event.outcome.status==XR_XIR_CALL_BOUNDS && event.outcome.panic.detail.code==XR_XIR_PANIC_BOUNDS && event.outcome.panic.detail.index==iterator_cold_index(spec->kind) && !event.outcome.panic.detail.length);return true;}
    if(spec->kind==4){CHECK(event.outcome.status==XR_XIR_CALL_RUNTIME_PANIC && event.outcome.panic.detail.code==XR_XIR_PANIC_ITERATOR_EXHAUSTED);return true;}
    CHECK(event.outcome.status==XR_XIR_CALL_RETURNED);
    if(spec->kind==1){CHECK(event.outcome.value.type==XR_XIR_BOOL && event.outcome.value.payload==1);return true;}
    unsigned element=spec->kind==3?1u:0u;iterator_text(&event.outcome.value,element?"birch":"cedar");
    XrXirValue *held=&r->traces[spec->victim].held[element];XrXirValueStatus status=xr_xir_value_copy(&event.outcome.value,held);
    if(status==XR_XIR_VALUE_OK)return true;
    CHECK(status==(r->fault==XR_XIR_CALL_LIMIT?XR_XIR_VALUE_LIMIT:XR_XIR_VALUE_OOM) && !held->type && !held->reserved && !held->payload);r->complete=false;return false;
}
static void iterator_cold_policy(IteratorColdRun *run,const IteratorColdSpec *spec,XrXirInstanceResult event) {
    IteratorPrefixBudgetRun *h=&run->hot;IteratorResourceRun *r=&h->base;unsigned at=spec->victim,peer=1-at;
    XirGenerator *g=(XirGenerator *)r->addresses[at];h->progress.root_terminal=iterator_prefix_finished(event);
    for(unsigned i=0;i<2;++i){run->before_registered[i]=run->registered[i];run->before_entered[i]=run->entered[i];}
    h->progress.terminal=atomic_load_explicit(&g->state,memory_order_acquire)==XIR_GENERATOR_TERMINAL;h->progress.linked=g->consumer==r->instance->call;
    CHECK(!h->progress.terminal || (g->outcome.status==XR_XIR_CALL_RETURNED && run->registered[at]));
    h->expected[at]=h->progress.terminal?XR_XIR_CALL_RETURNED:XR_XIR_CALL_CANCELLED;
    h->expected[peer]=spec->mode?XR_XIR_CALL_CANCELLED:XR_XIR_CALL_RETURNED;
    run->expected_cleanup[at]=run->registered[at];run->expected_cleanup[peer]=spec->mode?run->registered[peer]:1u;
    CHECK(r->cleanup[at]<=run->expected_cleanup[at] && r->cleanup[peer]<=run->expected_cleanup[peer]);h->policy=true;
}
static bool iterator_cold_cancel(IteratorColdRun *run,const IteratorColdSpec *spec,XrXirInstanceResult before) {
    IteratorPrefixBudgetRun *h=&run->hot;IteratorResourceRun *r=&h->base;unsigned at=spec->victim,peer=1-at;
    XirGenerator *g=(XirGenerator *)r->addresses[at],*other=(XirGenerator *)r->addresses[peer];XrXirCall *peer_call=other->call,*victim_call=g->call;uint64_t peer_polls=other->accounting.polls,polls=g->accounting.polls;
    XrXirTaskWaitToken old={0};bool token=xr_xir_call_iterator_wait_token(r->instance->call,&old);
    XrXirCallStatus status=xr_xir_instance_cancel_current(r->instance);h->progress.control_status=status;
    CHECK(status==(h->progress.root_terminal?XR_XIR_CALL_BAD_STATE:XR_XIR_CALL_CANCEL_REQUESTED));
    if(before.outcome.status==XR_XIR_CALL_SUSPENDED)CHECK(xr_xir_instance_resume(r->instance,before.epoch,before.outcome.wake)==XR_XIR_CALL_BAD_STATE);
    if(token)CHECK(xr_xir_call_complete_iterator_wait(r->instance->call,&old)==XR_XIR_CALL_BAD_STATE);
    if(h->progress.root_terminal){XrXirInstanceResult after=iterator_cold_tick(run);if(!r->complete)return false;CHECK(after.outcome.status==before.outcome.status);}
    else {XrXirValue empty={0};bool done=iterator_resource_return(r,XR_XIR_CALL_CANCELLED,&empty);iterator_cold_observe(run);if(!done)return false;}
    if(h->progress.terminal)CHECK(g->outcome.status==XR_XIR_CALL_RETURNED);
    else if(h->progress.linked)CHECK(g->outcome.status==XR_XIR_CALL_CANCELLED && r->cleanup[at]==run->expected_cleanup[at]);
    else CHECK(g->call && g->accounting.polls==polls && !r->cleanup[at]);
    CHECK(other->call==peer_call && other->accounting.polls==peer_polls && !other->consumer && !r->cleanup[peer]);
    if(!spec->peer){bool warm=iterator_pull_first(r,peer);iterator_cold_observe(run);if(!warm)return false;}
    bool second=iterator_pull_second(r,peer,false);iterator_cold_observe(run);if(!second)return false;
    bool end=iterator_resource_end(r,peer);iterator_cold_observe(run);if(!end)return false;
    if(!h->progress.terminal && !h->progress.linked)CHECK(g->call==victim_call && g->accounting.polls==polls && !g->consumer && !r->cleanup[at]);
    CHECK(other->outcome.status==XR_XIR_CALL_RETURNED && r->cleanup[peer]==1);return true;
}
static bool iterator_cold_control(IteratorColdRun *run,const IteratorColdSpec *spec,XrXirInstanceResult event) {
    IteratorPrefixBudgetRun *h=&run->hot;IteratorResourceRun *r=&h->base;iterator_cold_policy(run,spec,event);
    if(h->progress.root_terminal && !iterator_cold_returned(run,spec,event))return false;
    if(!spec->mode)return iterator_cold_cancel(run,spec,event);
    XrXirTaskWaitToken old={0};bool token=xr_xir_call_iterator_wait_token(r->instance->call,&old);
    XrXirCallStatus status=spec->mode==1?xr_xir_instance_stop(r->instance):xr_xir_instance_free(r->instance);
    if(spec->mode==2)r->instance=NULL;h->progress.control_status=status;
    XrXirCallStatus expected=spec->mode==1 && h->progress.root_terminal?event.outcome.status:XR_XIR_CALL_READY;
    if(status!=expected && status!=r->fault)fprintf(stderr,"Cold stop kind=%u peer=%u victim=%u mode=%u prefix=%u status=%u expected=%u\n",spec->kind,spec->peer,spec->victim,spec->mode,spec->prefix,status,expected);
    if(!iterator_prefix_budget_status(h,status,expected))return false;iterator_cold_observe(run);
    if(r->instance) {
        XrXirInstanceResult after=iterator_cold_tick(run);if(!r->complete)return false;CHECK(after.outcome.status==(h->progress.root_terminal?event.outcome.status:XR_XIR_CALL_CANCELLED));
        if(event.outcome.status==XR_XIR_CALL_SUSPENDED)CHECK(xr_xir_instance_resume(r->instance,event.epoch,event.outcome.wake)==XR_XIR_CALL_BAD_STATE);
        if(token)CHECK(xr_xir_call_complete_iterator_wait(r->instance->call,&old)==XR_XIR_CALL_BAD_STATE);
    }
    for(unsigned i=0;i<2;++i){XirGenerator *g=(XirGenerator *)r->addresses[i];CHECK(g->outcome.status==h->expected[i] && !g->call && !g->consumer && r->cleanup[i]==run->expected_cleanup[i]);}
    return true;
}
static void iterator_cold_drive(IteratorColdRun *run,const IteratorColdSpec *spec) {
    IteratorPrefixBudgetRun *h=&run->hot;IteratorResourceRun *r=&h->base;
    bool made=iterator_resource_make(r);iterator_cold_observe(run);if(!made)return;
    if(spec->peer){bool warm=iterator_pull_first(r,1-spec->victim);iterator_cold_observe(run);if(!warm)return;CHECK(run->registered[1-spec->victim]);}
    CHECK(!run->registered[spec->victim] && !run->entered[spec->victim]);
    r->index=iterator_cold_index(spec->kind);if(!iterator_resource_start(r,spec->victim,iterator_cold_entry(spec->kind)))return;
    XrXirInstanceResult event={0};event.outcome.status=XR_XIR_CALL_READY;
    while(spec->mode==3?!iterator_prefix_finished(event):h->progress.polls<spec->prefix){CHECK(!iterator_prefix_finished(event) && h->progress.polls<4096);if(!iterator_cold_step(run,spec,&event))return;}
    if(spec->mode<3){CHECK(h->progress.polls==spec->prefix);(void)iterator_cold_control(run,spec,event);return;}
    if(!iterator_cold_returned(run,spec,event))return;
    XirGenerator *victim=(XirGenerator *)r->addresses[spec->victim];h->progress.root_terminal=1;
    h->progress.terminal=atomic_load_explicit(&victim->state,memory_order_acquire)==XIR_GENERATOR_TERMINAL;h->progress.linked=victim->consumer==r->instance->call;
    for(unsigned i=0;i<2;++i){XirGenerator *g=(XirGenerator *)r->addresses[i];h->expected[i]=atomic_load_explicit(&g->state,memory_order_acquire)==XIR_GENERATOR_TERMINAL?g->outcome.status:XR_XIR_CALL_CANCELLED;run->expected_cleanup[i]=run->registered[i];}h->policy=true;
}
static IteratorColdResult iterator_cold_operation(const IteratorColdSpec *spec,size_t failure,const XrXirInstanceConfig *limits) {
    CHECK(spec->backend<4 && spec->kind<7 && spec->peer<2 && spec->victim<2 && spec->mode<4);
    CHECK(!iterator_resource_borrowed && !runtime_live && !effects_compile_live);runtime_fail_at=SIZE_MAX;
    memset(iterator_cell_owner,0,sizeof(iterator_cell_owner));memset(iterator_cell_freed,0,sizeof(iterator_cell_freed));
    memset(iterator_resource_generator_owner,0,sizeof(iterator_resource_generator_owner));memset(iterator_resource_generator_frees,0,sizeof(iterator_resource_generator_frees));
    iterator_resource_final=(XrXirDomainBudgetStats){0};iterator_resource_previous=(XrXirDomainBudgetStats){0};iterator_resource_disposals=0;
    IteratorColdRun run={0};IteratorPrefixBudgetRun *h=&run.hot;IteratorResourceRun *r=&h->base;r->fault=limits?XR_XIR_CALL_LIMIT:XR_XIR_CALL_OOM;r->complete=true;r->pull=1;
    runtime_attempts=0;XrXirProgram *program=iterator_program(&r->fixture,spec->backend);IteratorColdResult result={0};result.resource.setup_sites=runtime_attempts;CHECK(!runtime_live && !runtime_bytes);
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);if(limits)config=*limits;
    config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,iterator_resource_output,r};runtime_attempts=0;runtime_fail_at=failure;
    XrXirCallStatus status=xr_xir_instance_new(program,&config,&r->instance);r->complete=status==XR_XIR_CALL_READY;CHECK(r->complete || (status==r->fault && !r->instance));
    if(r->instance){r->had_domain=true;r->domain=r->instance->domain;iterator_resource_borrowed=r->domain;for(unsigned i=0;i<2;++i){r->traces[i].instance=r->instance;r->traces[i].fixture=&r->fixture;r->traces[i].index=i;}}
    xr_xir_compile_program_drop(program);CHECK(r->fixture.releases==(r->instance?0u:1u));
    if(r->complete)iterator_cold_drive(&run,spec);
    result.registered=spec->mode==3?run.registered[spec->victim]:run.before_registered[spec->victim];result.entered=spec->mode==3?run.entered[spec->victim]:run.before_entered[spec->victim];result.peer_registered=spec->mode==3?run.registered[1-spec->victim]:run.before_registered[1-spec->victim];result.progress=h->progress;result.policy=h->policy;
    iterator_cold_observe(&run);iterator_resource_retire(r);
    if(h->policy)for(unsigned i=0;i<2;++i)if(!run.expected_cleanup[i])CHECK(!r->cleanup[i]);
    if(r->complete)for(unsigned i=0;i<2;++i)CHECK(r->cleanup[i]==run.expected_cleanup[i]);
    iterator_prefix_budget_late(h);CHECK(!iterator_resource_borrowed && iterator_resource_disposals==(r->had_domain?1u:0u));
    result.resource.cost=iterator_resource_final;result.resource.complete=r->complete;result.resource.published=r->had_domain;result.resource.sites=runtime_attempts;result.resource.hit=failure!=SIZE_MAX && runtime_attempts>failure;
    result.resource.cleanup=r->cleanup[0]+r->cleanup[1];result.resource.cells=iterator_cell_freed[0]+iterator_cell_freed[1];result.resource.generators=iterator_resource_generator_frees[0]+iterator_resource_generator_frees[1];result.resource.busy=r->busy;result.resource.waits=r->waits;result.resource.phase=r->phase;
    CHECK(result.resource.cost.requested_bytes<=config.requested_value_limit && result.resource.cost.requested_call_bytes<=config.requested_call_limit && result.resource.cost.work<=config.work_limit);
    runtime_fail_at=SIZE_MAX;CHECK(!runtime_live && !runtime_bytes && !effects_compile_live && !effects_compile_bytes);return result;
}
static IteratorColdResult iterator_cold_baseline(const IteratorColdSpec *spec) {
    IteratorColdResult r=iterator_cold_operation(spec,SIZE_MAX,NULL);
    CHECK(r.resource.complete && r.resource.published && !r.resource.setup_sites && r.resource.sites && r.resource.sites<20000 && r.resource.cells==2 && r.resource.generators==2 && r.resource.busy==r.resource.cleanup*4);
    printf("Iterator cold resources backend=%u kind=%u peer=%u victim=%u mode=%u prefix=%u polls=%u registered=%u entered=%u peerRegistered=%u linked=%u terminal=%u rootTerminal=%u controlStatus=%u sites=%zu setupSites=%zu value=%llu call=%llu work=%llu waits=%u cleanup=%u busy=%u actualFrontier originalDomain physical0/0\n",spec->backend,spec->kind,spec->peer,spec->victim,spec->mode,spec->prefix,r.progress.polls,r.registered,r.entered,r.peer_registered,r.progress.linked,r.progress.terminal,r.progress.root_terminal,r.progress.control_status,r.resource.sites,r.resource.setup_sites,(unsigned long long)r.resource.cost.requested_bytes,(unsigned long long)r.resource.cost.requested_call_bytes,(unsigned long long)r.resource.cost.work,r.resource.waits,r.resource.cleanup,r.resource.busy);return r;
}
static void iterator_cold_axes(const IteratorColdSpec *spec,const IteratorColdResult *normal) {
    const XrXirDomainBudgetStats *cost=&normal->resource.cost;const uint64_t values[]={cost->requested_bytes,cost->requested_call_bytes,cost->work};
    for(unsigned axis=0;axis<3;++axis)for(unsigned minus=0;minus<2;++minus){XrXirInstanceConfig config;CHECK(values[axis]>1 && xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);if(!axis)config.requested_value_limit=values[axis]-minus;else if(axis==1)config.requested_call_limit=values[axis]-minus;else config.work_limit=values[axis]-minus;
        IteratorColdResult r=iterator_cold_operation(spec,SIZE_MAX,&config);CHECK(r.resource.complete==!minus);
        if(!minus)CHECK(r.progress.polls==normal->progress.polls && r.registered==normal->registered && r.entered==normal->entered && r.peer_registered==normal->peer_registered && r.progress.linked==normal->progress.linked && r.progress.terminal==normal->progress.terminal && r.progress.root_terminal==normal->progress.root_terminal && r.progress.control_status==normal->progress.control_status && r.policy==normal->policy && r.resource.cleanup==normal->resource.cleanup && r.resource.busy==normal->resource.busy && r.resource.cost.requested_bytes==cost->requested_bytes && r.resource.cost.requested_call_bytes==cost->requested_call_bytes && r.resource.cost.work==cost->work);
        printf("Iterator cold axis backend=%u kind=%u peer=%u victim=%u mode=%u prefix=%u axis=%u minus=%u complete=%u value=%llu call=%llu work=%llu physical0/0\n",spec->backend,spec->kind,spec->peer,spec->victim,spec->mode,spec->prefix,axis,minus,r.resource.complete,(unsigned long long)r.resource.cost.requested_bytes,(unsigned long long)r.resource.cost.requested_call_bytes,(unsigned long long)r.resource.cost.work);
    }
}
#endif // XIR_MIXED_ITERATOR_COLD_PREFIX_RESOURCES_H
