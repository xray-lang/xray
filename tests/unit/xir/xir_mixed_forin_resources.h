/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_mixed_forin_resources.h - Original owner transactions across lexical iterator control
 */
#ifndef XIR_MIXED_FORIN_RESOURCES_H
#define XIR_MIXED_FORIN_RESOURCES_H
#define main forin_resource_original_main
#include "test_xir_mixed_forin_lifetime.c"
#undef main
typedef struct ForinResourceSpec { unsigned backend,scenario,mode,prefix,close,drop; } ForinResourceSpec;
typedef struct ForinResourceRegistration { uintptr_t address; bool entered,registered; } ForinResourceRegistration;
typedef struct ForinResourceWitness {
    ForinResourceRegistration producers[8];
    unsigned count,root_registered,current_root_frontier;
} ForinResourceWitness;
static const unsigned forin_resource_polls[12]={52,88,50,95,66,64,118,102,130,129,116,143};
static bool forin_resource_terminal(XrXirInstanceResult event) {
    return event.outcome.status!=XR_XIR_CALL_READY && event.outcome.status!=XR_XIR_CALL_SUSPENDED;
}
static void forin_resource_observe(ForinTrace *trace,ForinResourceWitness *witness) {
    CHECK(trace->instance && !trace->instance->driving);witness->current_root_frontier=0;
    for(const CallFrame *frame=trace->instance->call->top;frame;frame=frame->parent) {
        unsigned entry=(unsigned)(frame->entry-trace->instance->call->config.entries);
        bool selected=entry==p20_iterator_lifetime_entries[1] || entry==p20_iterator_lifetime_entries[2] || entry==p20_iterator_lifetime_entries[3];
        if(!selected)continue;
        witness->current_root_frontier=frame->cleanup_frontier;
        if(frame->cleanup_frontier){CHECK(frame->entered);if(entry==p20_iterator_lifetime_entries[3])witness->root_registered=1;}
    }
    for(const XirGenerator *g=trace->instance->executor->generators;g;g=g->active_next) {
        unsigned at=0;while(at<witness->count && witness->producers[at].address!=(uintptr_t)g)++at;
        if(at==witness->count){CHECK(at<8);witness->producers[at].address=(uintptr_t)g;++witness->count;}
        if(!g->call)continue;
        for(const CallFrame *frame=g->call->top;frame;frame=frame->parent)if(frame->entry->flags&XR_XIR_ENTRY_GENERATOR) {
            witness->producers[at].entered|=frame->entered;
            if(frame->cleanup_frontier){CHECK(frame->entered);witness->producers[at].registered=true;}
        }
    }
}
static unsigned forin_resource_prints(const ForinTrace *trace,unsigned value) {
    unsigned count=0;for(unsigned i=0;i<trace->output_count;++i)count+=trace->output[i]==value;return count;
}
static void forin_resource_resume(ForinTrace traces[2],unsigned owner,XrXirInstanceResult event,unsigned scenario) {
    if(traces[1-owner].instance && traces[1-owner].instance->call){forin_resume(traces,owner,event,scenario);return;}
    ForinTrace *trace=&traces[owner];XrXirWaitRequest request={0};
    CHECK(event.epoch && event.outcome.wake && xr_xir_instance_wait_request(trace->instance,event.epoch,event.outcome.wake,&request)==XR_XIR_CALL_READY);
    CHECK(trace->timers<forin_timer_count[scenario] && request.kind==XR_XIR_WAIT_TIMER_MS && !request.reserved);
    CHECK(request.after_ms==forin_timers[scenario][trace->timers++]);
    CHECK(xr_xir_instance_resume(trace->instance,event.epoch+1,event.outcome.wake)==XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_instance_resume(trace->instance,event.epoch,event.outcome.wake+1)==XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_instance_resume(trace->instance,event.epoch,event.outcome.wake)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_resume(trace->instance,event.epoch,event.outcome.wake)==XR_XIR_CALL_BAD_STATE);
}
static void forin_resource_terminal_witness(const ForinTrace *trace,XrXirInstanceResult event,unsigned scenario) {
    if(scenario==5)CHECK(event.outcome.status==XR_XIR_CALL_BOUNDS && event.outcome.panic.detail.code==XR_XIR_PANIC_BOUNDS);
    else {
        CHECK(event.outcome.status==XR_XIR_CALL_RETURNED);
        if(scenario==1)CHECK(event.outcome.value.type==XR_XIR_BOOL && event.outcome.value.payload==1);
        else iterator_text(&event.outcome.value,forin_expected[scenario]);
    }
    const unsigned ordinary[]={701,scenario==0?801u:scenario==1?802u:810u},nested[]={701,820,701,810},alias[]={701,820,810};
    const unsigned *expected=scenario==6?nested:scenario==7?alias:ordinary;unsigned count=scenario==6?4:scenario==7?3:2;
    CHECK(trace->output_count==count && !memcmp(trace->output,expected,count*sizeof(*expected)));
    CHECK(trace->timers==forin_timer_count[scenario] && trace->root_mask==(iterator_native(trace->fixture->backend,true)?2u:1u));
    CHECK(trace->producer_mask==(iterator_native(trace->fixture->backend,false)?2u:1u) && trace->cell_count);
    const XirGenerator *g=(const XirGenerator *)(uintptr_t)trace->iterator.payload;CHECK(!g->consumer);
    if(scenario<10)CHECK(!g->call);else CHECK(!g->accounting.polls);
}
static unsigned forin_resource_number(const char *text) {
    char *end=NULL;unsigned long n=strtoul(text,&end,10);CHECK(text[0] && end && !*end && n<20000);return(unsigned)n;
}
typedef struct ForinResourceResult {
    XrXirDomainBudgetStats cost[2];
    XrXirDomainBudgetStats fault_cost[2];
    size_t sites,setup;
    unsigned phase,polls,published,disposals,entered,registered,root_registered,root_frontier,root_mask;
    unsigned terminal,control,fault,fault_owner,fault_phase,fault_observed,published_mask,outputs[2][8],output_count[2];
    bool unaffected[2];
    bool complete,hit,policy;
} ForinResourceResult;
typedef enum ForinResourceStage {
    FORIN_RESOURCE_NEW,FORIN_RESOURCE_FACTORY,FORIN_RESOURCE_ITERATOR,
    FORIN_RESOURCE_ROOT,FORIN_RESOURCE_TERMINAL,FORIN_RESOURCE_CONTROLLED
} ForinResourceStage;
typedef struct ForinResourceRun {
    IteratorFixture fixture;
    ForinTrace traces[2];
    ForinResourceWitness witness;
    XrXirCallStatus fault,accepted_fault;
    XrXirInstanceResult last[2];
    XrXirDomainBudgetStats fault_cost[2];
    ForinResourceStage stage[2];
    unsigned phase,polls,entered,registered,root_registered,root_frontier,root_mask,terminal,control;
    unsigned owner,first_fault_owner,first_fault_phase,fault_observed;
    unsigned committed[8],committed_count,pending,root_pending;
    bool complete,published[2],faulted[2],selected,policy;
} ForinResourceRun;
static void forin_resource_fault(ForinResourceRun *run) {
    CHECK(run->owner<2);run->complete=false;run->faulted[run->owner]=true;
    if(run->accepted_fault!=XR_XIR_CALL_READY)return;
    run->accepted_fault=run->fault;run->first_fault_owner=run->owner;run->first_fault_phase=run->phase;
    for(unsigned owner=0;owner<2;++owner) {
        if(forin_borrowed_domain[owner]){run->fault_cost[owner]=xr_xir_domain_budget_stats(forin_borrowed_domain[owner]);run->fault_observed|=1u<<owner;}
        else if(forin_domain_disposals[owner]){run->fault_cost[owner]=forin_final_cost[owner];run->fault_observed|=1u<<owner;}
    }
}
static bool forin_resource_status(ForinResourceRun *run,XrXirCallStatus status,XrXirCallStatus expected) {
    if(status==run->fault){forin_resource_fault(run);return false;}
    if(status!=expected)fprintf(stderr,"For-in resources phase=%u actual=%u expected=%u fault=%u\n",run->phase,status,expected,run->fault);
    CHECK(status==expected);return true;
}
static bool forin_resource_value_status(ForinResourceRun *run,XrXirValueStatus status) {
    if(status==XR_XIR_VALUE_OK)return true;
    CHECK(status==(run->fault==XR_XIR_CALL_LIMIT?XR_XIR_VALUE_LIMIT:XR_XIR_VALUE_OOM));
    forin_resource_fault(run);return false;
}
static XrXirInstanceResult forin_resource_tick(ForinResourceRun *run,unsigned owner) {
    run->owner=owner;XrXirInstanceResult event=forin_tick(run->traces,owner);run->last[owner]=event;
    if(event.outcome.status==run->fault)forin_resource_fault(run);
    else CHECK(event.outcome.status==XR_XIR_CALL_READY || event.outcome.status==XR_XIR_CALL_SUSPENDED || event.outcome.status==XR_XIR_CALL_RETURNED || event.outcome.status==XR_XIR_CALL_BOUNDS || event.outcome.status==XR_XIR_CALL_CANCELLED);
    CHECK(xr_xir_call_result_valid(&event.outcome));return event;
}
static bool forin_resource_make(ForinResourceRun *run,unsigned owner) {
    ++run->phase;run->owner=owner;run->stage[owner]=FORIN_RESOURCE_FACTORY;ForinTrace *trace=&run->traces[owner];
    if(!forin_resource_status(run,xr_xir_instance_start(trace->instance,p20_iterator_lifetime_entries[0],NULL,0),XR_XIR_CALL_READY))return false;
    XrXirInstanceResult event={0};unsigned polls=0;
    do{event=forin_resource_tick(run,owner);CHECK(++polls<4096);if(!run->complete)return false;}while(event.outcome.status==XR_XIR_CALL_READY);
    CHECK(event.outcome.status==XR_XIR_CALL_RETURNED && !trace->output_count);
    if(!forin_resource_status(run,xr_xir_instance_take_result(trace->instance,&trace->iterator),XR_XIR_CALL_RETURNED))return false;
    CHECK(xr_xir_value_valid(&trace->iterator));
    if(!forin_resource_value_status(run,xr_xir_value_copy(&trace->iterator,&trace->alias)))return false;
    const XirGenerator *g=(const XirGenerator *)(uintptr_t)trace->iterator.payload;
    CHECK(g->object.kind==XR_XIR_TYPE_ITERATOR && g->object.domain==forin_borrowed_domain[owner] && !g->consumer && !g->accounting.polls);
    trace->root_mask=trace->producer_mask=trace->polls=0;run->stage[owner]=FORIN_RESOURCE_ITERATOR;return true;
}
static bool forin_resource_start_root(ForinResourceRun *run,const ForinResourceSpec *spec,unsigned owner) {
    ++run->phase;run->owner=owner;ForinTrace *trace=&run->traces[owner];
        if(spec->mode==3 && run->traces[1-owner].iterator.type) {
            XrXirValue foreign[]={run->traces[1-owner].iterator,{XR_XIR_I64,0,0}};
            uint64_t epoch=trace->instance->epoch;size_t attempts=runtime_attempts;
            if(!forin_resource_status(run,xr_xir_instance_start(trace->instance,p20_iterator_lifetime_entries[3],foreign,2),XR_XIR_CALL_BAD_ARGUMENT))return false;
            CHECK(epoch==trace->instance->epoch && attempts==runtime_attempts && !trace->output_count);
        }
        XrXirValue args[]={trace->iterator,{XR_XIR_I64,0,spec->scenario>=2?spec->scenario-2:0}};
        unsigned operation=spec->scenario<2?spec->scenario+1:3;
        if(!forin_resource_status(run,xr_xir_instance_start(trace->instance,p20_iterator_lifetime_entries[operation],args,operation==3?2:1),XR_XIR_CALL_READY))return false;
    run->last[owner]=(XrXirInstanceResult){.outcome={.status=XR_XIR_CALL_READY}};run->stage[owner]=FORIN_RESOURCE_ROOT;return true;
}
static bool forin_resource_start(ForinResourceRun *run,const ForinResourceSpec *spec) {
    for(unsigned owner=0;owner<2;++owner)if(!forin_resource_make(run,owner))return false;
    CHECK(run->traces[0].iterator.payload!=run->traces[1].iterator.payload);
    for(unsigned owner=0;owner<2;++owner)if(!forin_resource_start_root(run,spec,owner))return false;
    run->selected=true;return true;
}
static bool forin_resource_take(ForinResourceRun *run,unsigned owner,XrXirInstanceResult event,unsigned scenario) {
    run->owner=owner;ForinTrace *trace=&run->traces[owner];forin_resource_terminal_witness(trace,event,scenario);
    if(scenario==5) {
        CHECK(xr_xir_instance_take_result(trace->instance,&trace->value)==XR_XIR_CALL_BAD_STATE && !trace->value.type && !trace->value.payload);
    }else {
        if(!forin_resource_status(run,xr_xir_instance_take_result(trace->instance,&trace->value),XR_XIR_CALL_RETURNED))return false;
        if(scenario==1)CHECK(trace->value.type==XR_XIR_BOOL && trace->value.payload==1);
        else iterator_text(&trace->value,forin_expected[scenario]);
    }
    trace->done=true;run->stage[owner]=FORIN_RESOURCE_TERMINAL;return true;
}
static void forin_resource_snapshot(ForinResourceRun *run,XrXirInstanceResult event) {
    ForinTrace *trace=&run->traces[0];run->policy=true;run->terminal=(unsigned)forin_resource_terminal(event);
    run->root_registered=run->witness.root_registered;run->root_frontier=run->witness.current_root_frontier;run->root_mask=trace->root_mask;
    for(unsigned i=0;i<run->witness.count;++i){run->entered+=(unsigned)run->witness.producers[i].entered;run->registered+=(unsigned)run->witness.producers[i].registered;}
    run->committed_count=trace->output_count;memcpy(run->committed,trace->output,sizeof(run->committed));
    run->pending=run->registered-forin_resource_prints(trace,701);run->root_pending=run->root_registered-forin_resource_prints(trace,810);
    CHECK(run->pending<=2 && run->root_pending<=1);
}
static void forin_resource_committed(const ForinResourceRun *run) {
    if(!run->policy)return;const ForinTrace *trace=&run->traces[0];
    CHECK(trace->output_count>=run->committed_count && !memcmp(trace->output,run->committed,run->committed_count*sizeof(*run->committed)));
    if(!run->complete) {
        unsigned producers=0,root=0;
        for(unsigned i=run->committed_count;i<trace->output_count;++i) {
            unsigned value=trace->output[i];CHECK(value==701 || value==810);
            if(value==701){CHECK(!root);++producers;}else ++root;
        }
        CHECK(producers<=run->pending && root<=run->root_pending);return;
    }
    CHECK(trace->output_count==run->committed_count+run->pending+run->root_pending);
    for(unsigned i=0;i<run->pending;++i)CHECK(trace->output[run->committed_count+i]==701);
    if(run->root_pending)CHECK(trace->output[run->committed_count+run->pending]==810);
}
static bool forin_resource_control(ForinResourceRun *run,const ForinResourceSpec *spec,XrXirInstanceResult event) {
    ++run->phase;run->owner=0;ForinTrace *trace=&run->traces[0];forin_resource_snapshot(run,event);
    if(run->terminal)forin_resource_terminal_witness(trace,event,spec->scenario);
    if(run->terminal && spec->mode==2 && event.outcome.status==XR_XIR_CALL_RETURNED)
        if(!forin_resource_value_status(run,xr_xir_value_copy(&event.outcome.value,&trace->value)))return false;
    unsigned active=0;for(const XirGenerator *g=trace->instance->executor->generators;g;g=g->active_next)active+=g->call!=NULL;
    XrXirTaskWaitToken tokens[8];unsigned token_count=0;
    for(XirGenerator *g=trace->instance->executor->generators;g;g=g->active_next)if(g->consumer){CHECK(token_count<8);if(xr_xir_call_iterator_wait_token(g->consumer,&tokens[token_count]))++token_count;}
    XrXirDomainBudgetStats peer={0};if(forin_borrowed_domain[1])peer=xr_xir_domain_budget_stats(forin_borrowed_domain[1]);
    XrXirCallStatus status;
    if(!spec->mode) {
        status=xr_xir_instance_cancel_current(trace->instance);run->control=status;
        CHECK(status==(run->terminal?XR_XIR_CALL_BAD_STATE:XR_XIR_CALL_CANCEL_REQUESTED));
        if(!run->terminal) {
            XrXirInstanceResult last={0};unsigned polls=0;
            do{last=forin_resource_tick(run,0);CHECK(++polls<4096);if(!run->complete)return false;}while(last.outcome.status==XR_XIR_CALL_READY);
            CHECK(last.outcome.status==XR_XIR_CALL_CANCELLED);
        }
    }else {
        status=spec->mode==1?xr_xir_instance_stop(trace->instance):xr_xir_instance_free(trace->instance);
        if(spec->mode==2)trace->instance=NULL;run->control=status;forin_costs();
        if(forin_borrowed_domain[1]) {
            XrXirDomainBudgetStats after=xr_xir_domain_budget_stats(forin_borrowed_domain[1]);
            CHECK(peer.requested_bytes==after.requested_bytes && peer.requested_call_bytes==after.requested_call_bytes && peer.work==after.work);
        }
        if(!forin_resource_status(run,status,spec->mode==1 && run->terminal && active?event.outcome.status:XR_XIR_CALL_READY))return false;
    }
    if(trace->instance) {
        if(event.outcome.status==XR_XIR_CALL_SUSPENDED)CHECK(xr_xir_instance_resume(trace->instance,event.epoch,event.outcome.wake)==XR_XIR_CALL_BAD_STATE);
        for(unsigned i=0;i<token_count;++i)CHECK(xr_xir_call_complete_iterator_wait(trace->instance->call,&tokens[i])==XR_XIR_CALL_BAD_STATE);
        XrXirInstanceResult after=forin_resource_tick(run,0);if(!run->complete)return false;
        CHECK(after.outcome.status==(run->terminal?event.outcome.status:XR_XIR_CALL_CANCELLED));
        if(run->terminal)CHECK(after.outcome.value.type==event.outcome.value.type && after.outcome.value.payload==event.outcome.value.payload && after.outcome.panic.detail.code==event.outcome.panic.detail.code);
        if(run->terminal && event.outcome.status==XR_XIR_CALL_RETURNED)
            if(!forin_resource_status(run,xr_xir_instance_take_result(trace->instance,&trace->value),XR_XIR_CALL_RETURNED))return false;
    }
    XrXirDomainBudgetStats after={0};if(forin_borrowed_domain[1])after=xr_xir_domain_budget_stats(forin_borrowed_domain[1]);
    CHECK(peer.requested_bytes==after.requested_bytes && peer.requested_call_bytes==after.requested_call_bytes && peer.work==after.work);
    forin_resource_committed(run);run->stage[0]=FORIN_RESOURCE_CONTROLLED;return true;
}
static void forin_resource_drive(ForinResourceRun *run,const ForinResourceSpec *spec) {
    if(!forin_resource_start(run,spec))return;
    XrXirInstanceResult events[2]={{.outcome={.status=XR_XIR_CALL_READY}},{.outcome={.status=XR_XIR_CALL_READY}}};
    if(spec->mode==3) {
        for(unsigned turns=0;!run->traces[0].done || !run->traces[1].done;++turns) {
            CHECK(turns<4096);
            for(unsigned owner=0;owner<2;++owner)if(!run->traces[owner].done) {
                events[owner]=forin_resource_tick(run,owner);if(!run->complete)return;
                if(events[owner].outcome.status==XR_XIR_CALL_SUSPENDED) {
                    forin_resume(run->traces,owner,events[owner],spec->scenario);run->last[owner].outcome.status=XR_XIR_CALL_READY;
                }
                else if(forin_resource_terminal(events[owner]) && !forin_resource_take(run,owner,events[owner],spec->scenario))return;
            }
        }
        for(unsigned a=0;a<run->traces[0].cell_count;++a)for(unsigned b=0;b<run->traces[1].cell_count;++b)
            CHECK(run->traces[0].cells[a]!=run->traces[1].cells[b]);
        for(unsigned owner=0;owner<2;++owner)CHECK(run->traces[owner].polls==forin_resource_polls[spec->scenario]);
        run->polls=run->traces[0].polls;run->root_mask=run->traces[0].root_mask;run->terminal=1;
        return;
    }
    forin_resource_observe(&run->traces[0],&run->witness);
    while(run->polls<spec->prefix) {
        CHECK(!forin_resource_terminal(events[0]));if(events[0].outcome.status==XR_XIR_CALL_SUSPENDED)forin_resource_resume(run->traces,0,events[0],spec->scenario);
        events[0]=forin_resource_tick(run,0);++run->polls;if(!run->complete)return;
        forin_resource_observe(&run->traces[0],&run->witness);
    }
    if(!forin_resource_control(run,spec,events[0]))return;
    ++run->phase;
    do {
        if(events[1].outcome.status==XR_XIR_CALL_SUSPENDED)forin_resource_resume(run->traces,1,events[1],spec->scenario);
        events[1]=forin_resource_tick(run,1);if(!run->complete)return;CHECK(run->traces[1].polls<=forin_resource_polls[spec->scenario]);
    }while(!forin_resource_terminal(events[1]));
    CHECK(run->traces[1].polls==forin_resource_polls[spec->scenario]);(void)forin_resource_take(run,1,events[1],spec->scenario);
}
/* The accepted failure stays recorded while the unaffected Instance uses its original ledger. */
static void forin_resource_unaffected(ForinResourceRun *run,const ForinResourceSpec *spec,unsigned owner) {
    CHECK(!run->complete && run->accepted_fault==run->fault && run->first_fault_owner<2 && owner!=run->first_fault_owner);
    CHECK(run->published[owner] && !run->faulted[owner]);
    if(run->stage[owner]>=FORIN_RESOURCE_TERMINAL)return;
    run->complete=true;
    if(run->stage[owner]==FORIN_RESOURCE_NEW)CHECK(forin_resource_make(run,owner));
    CHECK(run->stage[owner]>=FORIN_RESOURCE_ITERATOR);
    if(run->stage[owner]==FORIN_RESOURCE_ITERATOR)CHECK(forin_resource_start_root(run,spec,owner));
    CHECK(run->stage[owner]==FORIN_RESOURCE_ROOT && run->complete && !run->faulted[owner]);
    XrXirInstanceResult event=run->last[owner];
    if(!owner && spec->mode<3) {
        run->selected=true;forin_resource_observe(&run->traces[0],&run->witness);
        while(run->traces[0].polls<spec->prefix) {
            CHECK(!forin_resource_terminal(event));
            if(event.outcome.status==XR_XIR_CALL_SUSPENDED)forin_resource_resume(run->traces,0,event,spec->scenario);
            event=forin_resource_tick(run,0);CHECK(run->complete && !run->faulted[0]);
            forin_resource_observe(&run->traces[0],&run->witness);
        }
        run->polls=run->traces[0].polls;CHECK(run->polls==spec->prefix && forin_resource_control(run,spec,event));
    }else {
        unsigned polls=0;
        while(!forin_resource_terminal(event)) {
            if(event.outcome.status==XR_XIR_CALL_SUSPENDED)forin_resource_resume(run->traces,owner,event,spec->scenario);
            event=forin_resource_tick(run,owner);CHECK(run->complete && !run->faulted[owner] && ++polls<4096);
        }
        CHECK(run->traces[owner].polls==forin_resource_polls[spec->scenario]);
        CHECK(forin_resource_take(run,owner,event,spec->scenario));
    }
    CHECK(run->complete && !run->faulted[owner] && run->stage[owner]>=FORIN_RESOURCE_TERMINAL);
    run->complete=false;
}
static void forin_resource_retire(ForinResourceRun *run,const ForinResourceSpec *spec) {
    for(unsigned n=0;n<2;++n) {
        unsigned owner=n?1-spec->close:spec->close;if(!run->traces[owner].instance)continue;
        run->owner=owner;XrXirCallStatus status=xr_xir_instance_free(run->traces[owner].instance);run->traces[owner].instance=NULL;forin_costs();
        (void)forin_resource_status(run,status,XR_XIR_CALL_READY);
    }
    forin_resource_committed(run);CHECK(run->fixture.releases==1 && !run->fixture.vm && !run->fixture.entries);
    size_t attempts=runtime_attempts;
    for(unsigned n=0;n<2;++n) {
        unsigned owner=n?1-spec->drop:spec->drop;ForinTrace *trace=&run->traces[owner];
        if(trace->iterator.type) {
            uintptr_t address=(uintptr_t)trace->iterator.payload;const XirGenerator *g=(const XirGenerator *)address;
            CHECK(xr_xir_value_valid(&trace->iterator) && !g->executor && !g->call && !g->consumer);
            bool alias=trace->alias.type!=0;CHECK(!alias || (trace->alias.type==trace->iterator.type && trace->alias.payload==trace->iterator.payload));
            xr_xir_value_drop(&trace->iterator);CHECK(iterator_physical(address)==alias);
            xr_xir_value_drop(&trace->alias);CHECK(!iterator_physical(address));
        }else CHECK(!trace->alias.type && !trace->alias.payload);
    }
    CHECK(attempts==runtime_attempts && !effects_compile_live && !effects_compile_bytes);
    for(unsigned owner=0;owner<2;++owner) {
        ForinTrace *trace=&run->traces[owner];if(trace->value.type && forin_expected[spec->scenario])iterator_text(&trace->value,forin_expected[spec->scenario]);xr_xir_value_drop(&trace->value);
    }
    CHECK(attempts==runtime_attempts);
}
static ForinResourceResult forin_resource_operation(const ForinResourceSpec *spec,size_t failure,const XrXirInstanceConfig *limits,unsigned limited_owner) {
    CHECK(spec->backend<4 && spec->scenario<12 && spec->mode<=3 && spec->close<2 && spec->drop<2 && limited_owner<2);
    CHECK(spec->mode==3 || spec->prefix<=forin_resource_polls[spec->scenario]);
    CHECK(!runtime_live && !runtime_bytes && !effects_compile_live && !forin_borrowed_domain[0] && !forin_borrowed_domain[1]);
    runtime_attempts=0;runtime_fail_at=SIZE_MAX;memset(forin_final_cost,0,sizeof(forin_final_cost));
    memset(forin_previous_cost,0,sizeof(forin_previous_cost));memset(forin_domain_disposals,0,sizeof(forin_domain_disposals));
    ForinResourceRun run={.fault=limits?XR_XIR_CALL_LIMIT:XR_XIR_CALL_OOM,.complete=true,.first_fault_owner=2};ForinResourceResult result={0};
    XrXirProgram *program=iterator_program(&run.fixture,spec->backend);result.setup=runtime_attempts;CHECK(!runtime_live && !runtime_bytes);
    XrXirInstanceConfig configs[2];for(unsigned owner=0;owner<2;++owner)CHECK(xr_xir_instance_config_init(&configs[owner],sizeof(configs[owner]))==XR_XIR_CALL_READY);
    if(limits)configs[limited_owner]=*limits;
    runtime_attempts=0;runtime_fail_at=failure;
    for(unsigned owner=0;owner<2;++owner) {
        run.owner=owner;ForinTrace *trace=&run.traces[owner];trace->fixture=&run.fixture;
        configs[owner].output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,forin_output,trace};
        if(!forin_resource_status(&run,xr_xir_instance_new(program,&configs[owner],&trace->instance),XR_XIR_CALL_READY)){CHECK(!trace->instance);continue;}
        run.published[owner]=true;forin_borrowed_domain[owner]=trace->instance->domain;forin_costs();
    }
    if(run.published[0] && run.published[1])CHECK(forin_borrowed_domain[0]!=forin_borrowed_domain[1]);
    xr_xir_compile_program_drop(program);if(run.complete)forin_resource_drive(&run,spec);
    if(!run.complete)for(unsigned owner=0;owner<2;++owner)if(run.published[owner] && !run.faulted[owner])forin_resource_unaffected(&run,spec,owner);
    forin_resource_retire(&run,spec);
    for(unsigned owner=0;owner<2;++owner) {
        CHECK(!forin_borrowed_domain[owner] && forin_domain_disposals[owner]==(unsigned)run.published[owner]);
        result.cost[owner]=forin_final_cost[owner];result.published+=(unsigned)run.published[owner];result.disposals+=forin_domain_disposals[owner];
        if(run.published[owner])result.published_mask|=1u<<owner;
        result.unaffected[owner]=!run.faulted[owner] && run.stage[owner]>=FORIN_RESOURCE_TERMINAL;result.fault_cost[owner]=run.fault_cost[owner];
        if(run.published[owner])CHECK(result.cost[owner].requested_bytes<=configs[owner].requested_value_limit && result.cost[owner].requested_call_bytes<=configs[owner].requested_call_limit && result.cost[owner].work<=configs[owner].work_limit);
        else CHECK(!result.cost[owner].requested_bytes && !result.cost[owner].requested_call_bytes && !result.cost[owner].work);
        result.output_count[owner]=run.traces[owner].output_count;memcpy(result.outputs[owner],run.traces[owner].output,sizeof(result.outputs[owner]));
    }
    result.sites=runtime_attempts;result.complete=run.complete;result.hit=failure!=SIZE_MAX && runtime_attempts>failure;result.policy=run.policy;
    result.phase=run.phase;result.polls=run.polls;result.entered=run.entered;result.registered=run.registered;result.root_registered=run.root_registered;result.root_frontier=run.root_frontier;result.root_mask=run.root_mask;
    result.terminal=run.terminal;result.control=run.control;result.fault=run.accepted_fault;result.fault_owner=run.first_fault_owner;result.fault_phase=run.first_fault_phase;result.fault_observed=run.fault_observed;runtime_fail_at=SIZE_MAX;
    CHECK(!runtime_live && !runtime_bytes && !effects_compile_live && !effects_compile_bytes);return result;
}
static ForinResourceResult forin_resource_baseline(const ForinResourceSpec *spec) {
    ForinResourceResult r=forin_resource_operation(spec,SIZE_MAX,NULL,0);
    CHECK(r.complete && r.published==2 && r.disposals==2 && !r.setup && r.sites && r.sites<20000 && !r.fault);
    CHECK(r.unaffected[0] && r.unaffected[1]);
    printf("For-in resources backend=%u scenario=%u mode=%u prefix=%u close=%u drop=%u sites=%zu polls=%u phase=%u policy=%u entered=%u registered=%u rootRegistered=%u rootFrontier=%u rootMask=%u terminal=%u control=%u value0=%llu call0=%llu work0=%llu value1=%llu call1=%llu work1=%llu originalDomains2 lastReferenceZero2 codeLease1 noAllocateDrop physical0/0\n",spec->backend,spec->scenario,spec->mode,spec->prefix,spec->close,spec->drop,r.sites,r.polls,r.phase,(unsigned)r.policy,r.entered,r.registered,r.root_registered,r.root_frontier,r.root_mask,r.terminal,r.control,(unsigned long long)r.cost[0].requested_bytes,(unsigned long long)r.cost[0].requested_call_bytes,(unsigned long long)r.cost[0].work,(unsigned long long)r.cost[1].requested_bytes,(unsigned long long)r.cost[1].requested_call_bytes,(unsigned long long)r.cost[1].work);return r;
}
static void forin_resource_axes(const ForinResourceSpec *spec,const ForinResourceResult *normal) {
    for(unsigned owner=0;owner<2;++owner) {
        const uint64_t fees[]={normal->cost[owner].requested_bytes,normal->cost[owner].requested_call_bytes,normal->cost[owner].work};
        for(unsigned axis=0;axis<3;++axis)for(unsigned minus=0;minus<2;++minus) {
            CHECK(fees[axis]>1);XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
            uint64_t cap=fees[axis]-minus;if(!axis)config.requested_value_limit=cap;else if(axis==1)config.requested_call_limit=cap;else config.work_limit=cap;
            ForinResourceResult r=forin_resource_operation(spec,SIZE_MAX,&config,owner);
            printf("For-in axis backend=%u scenario=%u mode=%u prefix=%u close=%u drop=%u owner=%u axis=%u minus=%u cap=%llu complete=%u fault=%u faultOwner=%u faultPhase=%u faultObserved=%u publishedMask=%u phase=%u unaffected0=%u unaffected1=%u firstWork0=%llu firstWork1=%llu value0=%llu call0=%llu work0=%llu value1=%llu call1=%llu work1=%llu physical0/0\n",spec->backend,spec->scenario,spec->mode,spec->prefix,spec->close,spec->drop,owner,axis,minus,(unsigned long long)cap,(unsigned)r.complete,r.fault,r.fault_owner,r.fault_phase,r.fault_observed,r.published_mask,r.phase,(unsigned)r.unaffected[0],(unsigned)r.unaffected[1],(unsigned long long)r.fault_cost[0].work,(unsigned long long)r.fault_cost[1].work,(unsigned long long)r.cost[0].requested_bytes,(unsigned long long)r.cost[0].requested_call_bytes,(unsigned long long)r.cost[0].work,(unsigned long long)r.cost[1].requested_bytes,(unsigned long long)r.cost[1].requested_call_bytes,(unsigned long long)r.cost[1].work);fflush(stdout);
            CHECK(r.complete==!minus && r.fault==(unsigned)(minus?XR_XIR_CALL_LIMIT:XR_XIR_CALL_READY));
            CHECK(r.unaffected[1-owner]);
            if(minus)CHECK(r.fault_owner==owner && r.cost[1-owner].requested_bytes==normal->cost[1-owner].requested_bytes && r.cost[1-owner].requested_call_bytes==normal->cost[1-owner].requested_call_bytes && r.cost[1-owner].work==normal->cost[1-owner].work);
            if(!minus) {
                CHECK(r.published==2 && r.disposals==2 && r.phase==normal->phase && r.polls==normal->polls && r.entered==normal->entered && r.registered==normal->registered);
                CHECK(r.root_registered==normal->root_registered && r.root_frontier==normal->root_frontier && r.root_mask==normal->root_mask && r.terminal==normal->terminal && r.control==normal->control && r.policy==normal->policy);
                CHECK(!memcmp(r.outputs,normal->outputs,sizeof(r.outputs)) && !memcmp(r.output_count,normal->output_count,sizeof(r.output_count)));
                for(unsigned i=0;i<2;++i)CHECK(r.cost[i].requested_bytes==normal->cost[i].requested_bytes && r.cost[i].requested_call_bytes==normal->cost[i].requested_call_bytes && r.cost[i].work==normal->cost[i].work);
            }
        }
    }
}
#endif // XIR_MIXED_FORIN_RESOURCES_H
