/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_mixed_iterator_prefix_resources.h - Original Domain transactions for every warm pull boundary
 */
#ifndef XIR_MIXED_ITERATOR_PREFIX_RESOURCES_H
#define XIR_MIXED_ITERATOR_PREFIX_RESOURCES_H
#include "xir_mixed_iterator_pull_resources.h"
#define main iterator_prefix_normal_original_main
#include "test_xir_mixed_iterator_pull_prefixes.c"
#undef main

typedef struct IteratorPrefixBudgetRun {
    IteratorResourceRun base;
    IteratorPrefixResult progress;
    XrXirCallStatus sticky[2],expected[2];
    bool seen[2],policy;
} IteratorPrefixBudgetRun;
typedef struct IteratorPrefixBudgetResult {
    IteratorResourceResult resource;
    IteratorPrefixResult progress;
    unsigned phase_mask;
} IteratorPrefixBudgetResult;

/* Once any authentic producer is terminal, a later failure cannot rewrite it. */
static void iterator_prefix_budget_capture(IteratorPrefixBudgetRun *run) {
    for(unsigned i=0;i<2;++i) {
        uintptr_t address=run->base.addresses[i];
        if(!address || iterator_resource_generator_owner[i]!=address) continue;
        XirGenerator *g=(XirGenerator *)address;
        if(run->seen[i]) CHECK(g->outcome.status==run->sticky[i] &&
            atomic_load_explicit(&g->state,memory_order_acquire)==XIR_GENERATOR_TERMINAL);
        if(atomic_load_explicit(&g->state,memory_order_acquire)!=XIR_GENERATOR_TERMINAL) continue;
        if(!run->seen[i]) {run->seen[i]=true;run->sticky[i]=g->outcome.status;}
        CHECK(g->outcome.status==XR_XIR_CALL_RETURNED || g->outcome.status==XR_XIR_CALL_CANCELLED || g->outcome.status==run->base.fault);
    }
}
static XrXirInstanceResult iterator_prefix_budget_tick(IteratorPrefixBudgetRun *run) {
    XrXirInstanceResult event=iterator_resource_tick(&run->base);iterator_prefix_budget_capture(run);return event;
}
static bool iterator_prefix_budget_status(IteratorPrefixBudgetRun *run,XrXirCallStatus status,XrXirCallStatus expected) {
    bool ok=iterator_resource_status(&run->base,status,expected);iterator_prefix_budget_capture(run);return ok;
}
static bool iterator_prefix_budget_step(IteratorPrefixBudgetRun *run,XrXirInstanceResult *event) {
    if(event->outcome.status==XR_XIR_CALL_SUSPENDED) iterator_resume(&run->base.traces[run->base.current],*event);
    *event=iterator_prefix_budget_tick(run);++run->progress.polls;
    if(!run->base.complete) return false;
    if(event->outcome.status==XR_XIR_CALL_SUSPENDED) {
        XrXirWaitRequest request={0};
        CHECK(xr_xir_instance_wait_request(run->base.instance,event->epoch,event->outcome.wake,&request)==XR_XIR_CALL_READY);
        CHECK(request.kind==XR_XIR_WAIT_TIMER_MS && request.after_ms==7 && !request.reserved);++run->base.waits;
    } else CHECK(event->outcome.status==XR_XIR_CALL_READY || event->outcome.status==XR_XIR_CALL_RETURNED || event->outcome.status==XR_XIR_CALL_RUNTIME_PANIC);
    return true;
}
static bool iterator_prefix_budget_root(IteratorPrefixBudgetRun *run,const IteratorPrefixSpec *spec,XrXirInstanceResult event) {
    CHECK(xr_xir_call_result_valid(&event.outcome));
    if(spec->kind==3) {
        CHECK(event.outcome.status==XR_XIR_CALL_RUNTIME_PANIC && event.outcome.panic.detail.code==XR_XIR_PANIC_ITERATOR_EXHAUSTED);return true;
    }
    CHECK(event.outcome.status==XR_XIR_CALL_RETURNED);
    if(spec->kind==1){CHECK(event.outcome.value.type==XR_XIR_BOOL && event.outcome.value.payload==1);return true;}
    iterator_text(&event.outcome.value,"birch");XrXirValue *held=&run->base.traces[spec->victim].held[1];
    XrXirValueStatus copied=xr_xir_value_copy(&event.outcome.value,held);
    if(copied==XR_XIR_VALUE_OK) return true;
    CHECK(copied==(run->base.fault==XR_XIR_CALL_LIMIT?XR_XIR_VALUE_LIMIT:XR_XIR_VALUE_OOM));
    CHECK(!held->type && !held->reserved && !held->payload);run->base.complete=false;return false;
}
static void iterator_prefix_budget_policy(IteratorPrefixBudgetRun *run,const IteratorPrefixSpec *spec,XrXirInstanceResult event) {
    IteratorResourceRun *r=&run->base;XirGenerator *g=(XirGenerator *)r->addresses[spec->victim];
    run->progress.root_terminal=iterator_prefix_finished(event);
    run->progress.terminal=atomic_load_explicit(&g->state,memory_order_acquire)==XIR_GENERATOR_TERMINAL;
    run->progress.linked=g->consumer==r->instance->call;
    CHECK(!run->progress.terminal || g->outcome.status==XR_XIR_CALL_RETURNED);
    run->expected[spec->victim]=run->progress.terminal?XR_XIR_CALL_RETURNED:XR_XIR_CALL_CANCELLED;
    run->expected[1-spec->victim]=spec->mode?XR_XIR_CALL_CANCELLED:XR_XIR_CALL_RETURNED;
    run->policy=true;iterator_prefix_budget_capture(run);
}
static bool iterator_prefix_budget_cancel(IteratorPrefixBudgetRun *run,const IteratorPrefixSpec *spec,XrXirInstanceResult before) {
    IteratorResourceRun *r=&run->base;IteratorPrefixResult *p=&run->progress;
    unsigned at=spec->victim,peer=1-at;XirGenerator *g=(XirGenerator *)r->addresses[at],*other=(XirGenerator *)r->addresses[peer];
    XrXirCall *peer_call=other->call;uint64_t peer_polls=other->accounting.polls,polls=g->accounting.polls;
    XrXirTaskWaitToken old={0};bool token=xr_xir_call_iterator_wait_token(r->instance->call,&old);
    XrXirCallStatus status=xr_xir_instance_cancel_current(r->instance);p->control_status=status;
    CHECK(status==(p->root_terminal?XR_XIR_CALL_BAD_STATE:XR_XIR_CALL_CANCEL_REQUESTED));
    if(before.outcome.status==XR_XIR_CALL_SUSPENDED) CHECK(xr_xir_instance_resume(r->instance,before.epoch,before.outcome.wake)==XR_XIR_CALL_BAD_STATE);
    if(token) CHECK(xr_xir_call_complete_iterator_wait(r->instance->call,&old)==XR_XIR_CALL_BAD_STATE);
    if(p->root_terminal) {
        XrXirInstanceResult after=iterator_prefix_budget_tick(run);if(!r->complete) return false;
        CHECK(after.outcome.status==before.outcome.status);
        if(spec->kind==3) CHECK(after.outcome.panic.detail.code==before.outcome.panic.detail.code);
        else if(spec->kind==1) CHECK(after.outcome.value.type==XR_XIR_BOOL && after.outcome.value.payload==1);
        else iterator_text(&after.outcome.value,"birch");
    } else {
        XrXirValue empty={0};bool returned=iterator_resource_return(r,XR_XIR_CALL_CANCELLED,&empty);iterator_prefix_budget_capture(run);
        if(!returned) return false;
    }
    if(p->terminal) CHECK(g->outcome.status==XR_XIR_CALL_RETURNED && r->cleanup[at]==1);
    else if(p->linked) CHECK(g->outcome.status==XR_XIR_CALL_CANCELLED && r->cleanup[at]==1);
    else CHECK(g->call && g->accounting.polls==polls && !r->cleanup[at]);
    CHECK(other->call==peer_call && other->accounting.polls==peer_polls && !other->consumer && !r->cleanup[peer]);
    bool second=iterator_pull_second(r,peer,false);iterator_prefix_budget_capture(run);if(!second) return false;
    bool end=iterator_resource_end(r,peer);iterator_prefix_budget_capture(run);if(!end) return false;
    CHECK(other->outcome.status==XR_XIR_CALL_RETURNED && r->cleanup[peer]==1);return true;
}
static bool iterator_prefix_budget_control(IteratorPrefixBudgetRun *run,const IteratorPrefixSpec *spec,XrXirInstanceResult event) {
    IteratorResourceRun *r=&run->base;IteratorPrefixResult *p=&run->progress;
    iterator_prefix_budget_policy(run,spec,event);
    if(p->root_terminal && !iterator_prefix_budget_root(run,spec,event)) return false;
    if(!spec->mode) return iterator_prefix_budget_cancel(run,spec,event);
    XrXirTaskWaitToken old={0};bool token=xr_xir_call_iterator_wait_token(r->instance->call,&old);
    XrXirCallStatus status=spec->mode==1?xr_xir_instance_stop(r->instance):xr_xir_instance_free(r->instance);
    if(spec->mode==2)r->instance=NULL;p->control_status=status;
    XrXirCallStatus expected=spec->mode==1 && p->root_terminal?event.outcome.status:XR_XIR_CALL_READY;
    if(!iterator_prefix_budget_status(run,status,expected)) return false;
    if(r->instance) {
        XrXirInstanceResult after=iterator_prefix_budget_tick(run);if(!r->complete) return false;
        CHECK(after.outcome.status==(p->root_terminal?event.outcome.status:XR_XIR_CALL_CANCELLED));
        if(p->root_terminal && spec->kind<3) {
            if(spec->kind==1)CHECK(after.outcome.value.type==XR_XIR_BOOL && after.outcome.value.payload==1);
            else iterator_text(&after.outcome.value,"birch");
        }
        if(event.outcome.status==XR_XIR_CALL_SUSPENDED)CHECK(xr_xir_instance_resume(r->instance,event.epoch,event.outcome.wake)==XR_XIR_CALL_BAD_STATE);
        if(token)CHECK(xr_xir_call_complete_iterator_wait(r->instance->call,&old)==XR_XIR_CALL_BAD_STATE);
    }
    for(unsigned i=0;i<2;++i) {
        XirGenerator *g=(XirGenerator *)r->addresses[i];
        CHECK(g->outcome.status==run->expected[i] && !g->call && !g->consumer && r->cleanup[i]==1);
    }
    return true;
}
static void iterator_prefix_budget_drive(IteratorPrefixBudgetRun *run,const IteratorPrefixSpec *spec) {
    IteratorResourceRun *r=&run->base;
    bool made=iterator_resource_make(r);iterator_prefix_budget_capture(run);if(!made)return;
    for(unsigned i=0;i<2;++i) {
        bool warm=iterator_pull_first(r,i);iterator_prefix_budget_capture(run);if(!warm)return;
        CHECK(r->traces[i].witness.root==(iterator_native(spec->backend,true)?2u:1u));
        CHECK(r->traces[i].witness.producer==(iterator_native(spec->backend,false)?2u:1u));
    }
    r->index=spec->kind==3?1:0;
    if(!iterator_resource_start(r,spec->victim,iterator_prefix_entry(spec->kind)))return;
    XrXirInstanceResult event={0};event.outcome.status=XR_XIR_CALL_READY;
    while(run->progress.polls<spec->prefix) {
        CHECK(!iterator_prefix_finished(event));if(!iterator_prefix_budget_step(run,&event))return;
    }
    CHECK(run->progress.polls==spec->prefix);(void)iterator_prefix_budget_control(run,spec,event);
}
/* Snapshot terminal facts again before retirement; later OOM never weakens them. */
static void iterator_prefix_budget_late(IteratorPrefixBudgetRun *run) {
    IteratorResourceRun *r=&run->base;CHECK(r->fixture.releases==1 && !r->fixture.vm && !r->fixture.entries);
    iterator_prefix_budget_capture(run);
    for(unsigned i=0;i<2;++i) {
        if(r->traces[i].iterator.type) {
            const XirGenerator *g=(const XirGenerator *)(uintptr_t)r->traces[i].iterator.payload;
            CHECK(xr_xir_value_valid(&r->traces[i].iterator) && !g->executor && !g->call && !g->consumer);
            if(run->seen[i]) CHECK(g->outcome.status==run->sticky[i]);
            if(r->complete){CHECK(run->policy);CHECK(g->outcome.status==run->expected[i]);}
            else CHECK(g->outcome.status==XR_XIR_CALL_RETURNED || g->outcome.status==XR_XIR_CALL_CANCELLED || g->outcome.status==r->fault);
        }
        size_t attempts=runtime_attempts;xr_xir_value_drop(&r->traces[i].iterator);
        if(r->traces[i].alias.type)CHECK(iterator_resource_generator_owner[i]==r->addresses[i] && iterator_physical(r->addresses[i]));
        xr_xir_value_drop(&r->traces[i].alias);CHECK(runtime_attempts==attempts);
        if(r->addresses[i]) CHECK(!iterator_resource_generator_owner[i] && iterator_resource_generator_frees[i]==1);
        if(r->traces[i].witness.cell) CHECK(iterator_cell_freed[i]==1 && !iterator_cell_owner[i]);
    }
    CHECK(!effects_compile_live && !effects_compile_bytes);
    for(unsigned i=0;i<2;++i)for(unsigned j=0;j<2;++j)if(r->traces[i].held[j].type){iterator_text(&r->traces[i].held[j],j?"birch":"cedar");xr_xir_value_drop(&r->traces[i].held[j]);}
}
static IteratorPrefixBudgetResult iterator_prefix_budget_operation(const IteratorPrefixSpec *spec,size_t failure,const XrXirInstanceConfig *limits) {
    CHECK(spec->backend<4 && spec->kind<4 && spec->victim<2 && spec->mode<3 && spec->prefix<=(spec->kind==3?34u:22u));
    CHECK(!iterator_resource_borrowed && !runtime_live && !effects_compile_live);runtime_fail_at=SIZE_MAX;
    memset(iterator_cell_owner,0,sizeof(iterator_cell_owner));memset(iterator_cell_freed,0,sizeof(iterator_cell_freed));
    memset(iterator_resource_generator_owner,0,sizeof(iterator_resource_generator_owner));memset(iterator_resource_generator_frees,0,sizeof(iterator_resource_generator_frees));
    iterator_resource_final=(XrXirDomainBudgetStats){0};iterator_resource_previous=(XrXirDomainBudgetStats){0};iterator_resource_disposals=0;
    IteratorPrefixBudgetRun run={0};run.base.fault=limits?XR_XIR_CALL_LIMIT:XR_XIR_CALL_OOM;run.base.complete=true;run.base.pull=1;
    IteratorResourceRun *r=&run.base;runtime_attempts=0;XrXirProgram *program=iterator_program(&r->fixture,spec->backend);
    IteratorPrefixBudgetResult result={0};result.resource.setup_sites=runtime_attempts;CHECK(!runtime_live && !runtime_bytes);
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);if(limits)config=*limits;
    config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,iterator_resource_output,r};runtime_attempts=0;runtime_fail_at=failure;
    XrXirCallStatus status=xr_xir_instance_new(program,&config,&r->instance);r->complete=status==XR_XIR_CALL_READY;
    CHECK(r->complete || (status==r->fault && !r->instance));
    if(r->instance) {
        r->had_domain=true;r->domain=r->instance->domain;iterator_resource_borrowed=r->domain;
        for(unsigned i=0;i<2;++i){r->traces[i].instance=r->instance;r->traces[i].fixture=&r->fixture;r->traces[i].index=i;}
    }
    xr_xir_compile_program_drop(program);CHECK(r->fixture.releases==(r->instance?0u:1u));
    if(r->complete)iterator_prefix_budget_drive(&run,spec);
    iterator_prefix_budget_capture(&run);iterator_resource_retire(r);iterator_prefix_budget_late(&run);
    CHECK(!iterator_resource_borrowed && iterator_resource_disposals==(r->had_domain?1u:0u));
    result.resource.cost=iterator_resource_final;result.resource.complete=r->complete;result.resource.published=r->had_domain;result.resource.sites=runtime_attempts;
    result.resource.hit=failure!=SIZE_MAX && runtime_attempts>failure;result.resource.cleanup=r->cleanup[0]+r->cleanup[1];result.resource.cells=iterator_cell_freed[0]+iterator_cell_freed[1];
    result.resource.generators=iterator_resource_generator_frees[0]+iterator_resource_generator_frees[1];result.resource.busy=r->busy;result.resource.waits=r->waits;result.resource.phase=r->phase;
    result.progress=run.progress;result.phase_mask=(run.policy?1u:0u)|(run.seen[0]?2u:0u)|(run.seen[1]?4u:0u);
    CHECK(result.resource.cost.requested_bytes<=config.requested_value_limit && result.resource.cost.requested_call_bytes<=config.requested_call_limit && result.resource.cost.work<=config.work_limit);
    runtime_fail_at=SIZE_MAX;CHECK(!runtime_live && !runtime_bytes && !effects_compile_live && !effects_compile_bytes);return result;
}
static IteratorPrefixBudgetResult iterator_prefix_budget_baseline(const IteratorPrefixSpec *spec) {
    IteratorPrefixBudgetResult r=iterator_prefix_budget_operation(spec,SIZE_MAX,NULL);IteratorResourceResult *v=&r.resource;
    CHECK(v->complete && v->published && !v->setup_sites && v->sites && v->sites<20000 && v->cleanup==2 && v->cells==2 && v->generators==2 && v->busy==8 && r.phase_mask==7);
    CHECK(r.progress.polls==spec->prefix);
    printf("Iterator prefix resources backend=%u kind=%u victim=%u mode=%u prefix=%u linked=%u terminal=%u rootTerminal=%u controlStatus=%u sites=%zu setupSites=%zu value=%llu call=%llu work=%llu waits=%u originalDomain noExtraRetain stickyBeforeControl stickyBeforeRetirement codeDead metadataBeforeStrings physical0/0\n",spec->backend,spec->kind,spec->victim,spec->mode,spec->prefix,r.progress.linked,r.progress.terminal,r.progress.root_terminal,r.progress.control_status,v->sites,v->setup_sites,(unsigned long long)v->cost.requested_bytes,(unsigned long long)v->cost.requested_call_bytes,(unsigned long long)v->cost.work,v->waits);return r;
}
static void iterator_prefix_budget_axes(const IteratorPrefixSpec *spec,const IteratorPrefixBudgetResult *normal) {
    const XrXirDomainBudgetStats *cost=&normal->resource.cost;const uint64_t values[]={cost->requested_bytes,cost->requested_call_bytes,cost->work};
    for(unsigned axis=0;axis<3;++axis)for(unsigned minus=0;minus<2;++minus) {
        XrXirInstanceConfig config;CHECK(values[axis]>1 && xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
        if(!axis)config.requested_value_limit=values[axis]-minus;else if(axis==1)config.requested_call_limit=values[axis]-minus;else config.work_limit=values[axis]-minus;
        IteratorPrefixBudgetResult r=iterator_prefix_budget_operation(spec,SIZE_MAX,&config);
        if(r.resource.complete!=!minus)fprintf(stderr,"Iterator prefix axis backend=%u kind=%u victim=%u mode=%u prefix=%u axis=%u minus=%u complete=%u phase=%u mask=%u\n",spec->backend,spec->kind,spec->victim,spec->mode,spec->prefix,axis,minus,r.resource.complete,r.resource.phase,r.phase_mask);
        CHECK(r.resource.complete==!minus);
        if(!minus) {
            CHECK(r.phase_mask==normal->phase_mask && r.progress.polls==normal->progress.polls && r.progress.linked==normal->progress.linked &&
                r.progress.terminal==normal->progress.terminal && r.progress.root_terminal==normal->progress.root_terminal && r.progress.control_status==normal->progress.control_status);
            CHECK(r.resource.cost.requested_bytes==cost->requested_bytes && r.resource.cost.requested_call_bytes==cost->requested_call_bytes && r.resource.cost.work==cost->work);
        }
        printf("Iterator prefix axis backend=%u kind=%u victim=%u mode=%u prefix=%u axis=%u minus=%u complete=%u value=%llu call=%llu work=%llu physical0/0\n",spec->backend,spec->kind,spec->victim,spec->mode,spec->prefix,axis,minus,r.resource.complete,(unsigned long long)r.resource.cost.requested_bytes,(unsigned long long)r.resource.cost.requested_call_bytes,(unsigned long long)r.resource.cost.work);
    }
}
#endif // XIR_MIXED_ITERATOR_PREFIX_RESOURCES_H
