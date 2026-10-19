/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_mixed_forin_prefixes.c - Registered lexical exits at every public poll boundary
 */
#define main forin_prefix_original_main
#include "test_xir_mixed_forin_lifetime.c"
#undef main
typedef struct ForinPrefixSpec { unsigned backend,scenario,mode,prefix,close,drop; } ForinPrefixSpec;
typedef struct ForinRegistration { uintptr_t address; bool entered,registered; } ForinRegistration;
typedef struct ForinPrefixWitness {
    ForinRegistration producers[8];
    unsigned count,root_entered,root_registered,current_root_entered,current_root_frontier;
} ForinPrefixWitness;
static const unsigned forin_measured_polls[12]={52,88,50,95,66,64,118,102,130,129,116,143};
static bool forin_terminal(XrXirInstanceResult event) {
    return event.outcome.status!=XR_XIR_CALL_READY && event.outcome.status!=XR_XIR_CALL_SUSPENDED;
}
static void forin_prefix_observe(ForinTrace *trace,ForinPrefixWitness *witness) {
    CHECK(trace->instance && !trace->instance->driving);
    witness->current_root_entered=witness->current_root_frontier=0;
    for(const CallFrame *f=trace->instance->call->top;f;f=f->parent) {
        unsigned entry=(unsigned)(f->entry-trace->instance->call->config.entries);
        if(entry==p20_iterator_lifetime_entries[1] || entry==p20_iterator_lifetime_entries[2] || entry==p20_iterator_lifetime_entries[3]) {
            witness->current_root_entered=(unsigned)f->entered;witness->current_root_frontier=f->cleanup_frontier;
        }
        if(entry==p20_iterator_lifetime_entries[3]) {
            witness->root_entered|=(unsigned)f->entered;
            if(f->cleanup_frontier){CHECK(f->entered);witness->root_registered=1;}
        }
    }
    for(XirGenerator *g=trace->instance->executor->generators;g;g=g->active_next) {
        unsigned at=0;while(at<witness->count && witness->producers[at].address!=(uintptr_t)g)++at;
        if(at==witness->count){CHECK(at<8);witness->producers[at].address=(uintptr_t)g;++witness->count;}
        if(!g->call)continue;
        for(const CallFrame *f=g->call->top;f;f=f->parent)if(f->entry->flags&XR_XIR_ENTRY_GENERATOR) {
            witness->producers[at].entered|=f->entered;
            if(f->cleanup_frontier){CHECK(f->entered);witness->producers[at].registered=true;}
        }
    }
}
static unsigned forin_output_count(const ForinTrace *trace,unsigned value) {
    unsigned count=0;for(unsigned i=0;i<trace->output_count;++i)count+=trace->output[i]==value;return count;
}
static void forin_prefix_resume(ForinTrace traces[2],unsigned at,XrXirInstanceResult event,unsigned scenario) {
    if(traces[1-at].instance){forin_resume(traces,at,event,scenario);return;}
    ForinTrace *trace=&traces[at];XrXirWaitRequest request={0};
    CHECK(event.epoch && event.outcome.wake && xr_xir_instance_wait_request(trace->instance,event.epoch,event.outcome.wake,&request)==XR_XIR_CALL_READY);
    CHECK(trace->timers<forin_timer_count[scenario] && request.kind==XR_XIR_WAIT_TIMER_MS && !request.reserved);
    CHECK(request.after_ms==forin_timers[scenario][trace->timers++]);
    CHECK(xr_xir_instance_resume(trace->instance,event.epoch+1,event.outcome.wake)==XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_instance_resume(trace->instance,event.epoch,event.outcome.wake+1)==XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_instance_resume(trace->instance,event.epoch,event.outcome.wake)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_resume(trace->instance,event.epoch,event.outcome.wake)==XR_XIR_CALL_BAD_STATE);
}
static void forin_prefix_sticky(ForinTrace traces[2],XrXirInstanceResult before,bool terminal) {
    XrXirInstanceResult after=forin_tick(traces,0);
    CHECK(after.outcome.status==(terminal?before.outcome.status:XR_XIR_CALL_CANCELLED));
    if(terminal) {
        CHECK(after.outcome.panic.detail.code==before.outcome.panic.detail.code);
        CHECK(after.outcome.value.type==before.outcome.value.type && after.outcome.value.payload==before.outcome.value.payload);
    }
}
static unsigned forin_prefix_control(ForinTrace traces[2],const ForinPrefixSpec *spec,XrXirInstanceResult before) {
    bool terminal=forin_terminal(before);XrXirTaskWaitToken tokens[8];unsigned count=0;
    unsigned active=0;
    for(const XirGenerator *g=traces[0].instance->executor->generators;g;g=g->active_next)active+=g->call!=NULL;
    for(XirGenerator *g=traces[0].instance->executor->generators;g;g=g->active_next)if(g->consumer) {
        CHECK(count<8);if(xr_xir_call_iterator_wait_token(g->consumer,&tokens[count]))++count;
    }
    XrXirDomainBudgetStats peer=xr_xir_domain_budget_stats(traces[1].instance->domain);
    XrXirCallStatus status;
    if(!spec->mode) {
        status=xr_xir_instance_cancel_current(traces[0].instance);
        CHECK(status==(terminal?XR_XIR_CALL_BAD_STATE:XR_XIR_CALL_CANCEL_REQUESTED));
        if(!terminal) {
            XrXirInstanceResult event={0};unsigned polls=0;
            do{event=forin_tick(traces,0);CHECK(++polls<4096);}while(event.outcome.status==XR_XIR_CALL_READY);
            CHECK(event.outcome.status==XR_XIR_CALL_CANCELLED);
        }
    }else {
        status=spec->mode==1?xr_xir_instance_stop(traces[0].instance):xr_xir_instance_free(traces[0].instance);
        if(spec->mode==2)traces[0].instance=NULL;
        XrXirCallStatus expected=spec->mode==1 && terminal && active?before.outcome.status:XR_XIR_CALL_READY;
        if(status!=expected)fprintf(stderr,"For-in control mode=%u terminal=%u active=%u before=%u actual=%u expected=%u\n",spec->mode,(unsigned)terminal,active,before.outcome.status,status,expected);
        CHECK(status==expected);
    }
    if(traces[0].instance) {
        if(before.outcome.status==XR_XIR_CALL_SUSPENDED)CHECK(xr_xir_instance_resume(traces[0].instance,before.epoch,before.outcome.wake)==XR_XIR_CALL_BAD_STATE);
        for(unsigned i=0;i<count;++i)CHECK(xr_xir_call_complete_iterator_wait(traces[0].instance->call,&tokens[i])==XR_XIR_CALL_BAD_STATE);
        forin_prefix_sticky(traces,before,terminal);
    }
    XrXirDomainBudgetStats after=xr_xir_domain_budget_stats(traces[1].instance->domain);
    CHECK(peer.requested_bytes==after.requested_bytes && peer.requested_call_bytes==after.requested_call_bytes && peer.work==after.work);
    return status;
}
static void forin_prefix_prepare(ForinTrace traces[2],IteratorFixture *fixture,unsigned backend,unsigned scenario) {
    CHECK(!runtime_live && !runtime_bytes && !effects_compile_live && !forin_borrowed_domain[0] && !forin_borrowed_domain[1]);
    runtime_fail_at=SIZE_MAX;runtime_attempts=0;memset(forin_final_cost,0,sizeof(forin_final_cost));
    memset(forin_previous_cost,0,sizeof(forin_previous_cost));memset(forin_domain_disposals,0,sizeof(forin_domain_disposals));
    XrXirProgram *program=iterator_program(fixture,backend);
    for(unsigned i=0;i<2;++i) {
        traces[i].fixture=fixture;XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
        config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,forin_output,&traces[i]};
        CHECK(xr_xir_instance_new(program,&config,&traces[i].instance)==XR_XIR_CALL_READY);
        forin_borrowed_domain[i]=traces[i].instance->domain;forin_costs();
    }
    CHECK(forin_borrowed_domain[0]!=forin_borrowed_domain[1]);xr_xir_compile_program_drop(program);CHECK(!fixture->releases);
    for(unsigned i=0;i<2;++i)forin_make(traces,i);
    for(unsigned i=0;i<2;++i) {
        XrXirValue args[]={traces[i].iterator,{XR_XIR_I64,0,scenario>=2?scenario-2:0}};unsigned operation=scenario<2?scenario+1:3;
        CHECK(xr_xir_instance_start(traces[i].instance,p20_iterator_lifetime_entries[operation],args,operation==3?2:1)==XR_XIR_CALL_READY);
    }
}
static void forin_prefix_retire(ForinTrace traces[2],IteratorFixture *fixture,const ForinPrefixSpec *spec) {
    for(unsigned n=0;n<2;++n) {
        unsigned at=n?1-spec->close:spec->close;if(!traces[at].instance)continue;
        CHECK(xr_xir_instance_free(traces[at].instance)==XR_XIR_CALL_READY);traces[at].instance=NULL;
    }
    CHECK(fixture->releases==1 && !fixture->vm && !fixture->entries);size_t sites=runtime_attempts;
    for(unsigned n=0;n<2;++n) {
        unsigned at=n?1-spec->drop:spec->drop;uintptr_t address=(uintptr_t)traces[at].iterator.payload;
        const XirGenerator *g=(const XirGenerator *)address;CHECK(xr_xir_value_valid(&traces[at].iterator) && !g->executor && !g->consumer && !g->call);
        xr_xir_value_drop(&traces[at].iterator);CHECK(iterator_physical(address));xr_xir_value_drop(&traces[at].alias);CHECK(!iterator_physical(address));
    }
    CHECK(sites==runtime_attempts && !effects_compile_live && !effects_compile_bytes);
    for(unsigned i=0;i<2;++i){if(traces[i].value.type && forin_expected[spec->scenario])iterator_text(&traces[i].value,forin_expected[spec->scenario]);xr_xir_value_drop(&traces[i].value);}
    CHECK(!runtime_live && !runtime_bytes && !forin_borrowed_domain[0] && !forin_borrowed_domain[1]);
    CHECK(forin_domain_disposals[0]==1 && forin_domain_disposals[1]==1 && runtime_attempts && runtime_attempts<20000);
}
static void forin_prefix_terminal_witness(const ForinTrace *trace,XrXirInstanceResult event,unsigned scenario) {
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
static void forin_prefix_operation(const ForinPrefixSpec *spec) {
    IteratorFixture fixture={0};ForinTrace traces[2]={{0}};ForinPrefixWitness witness={0};
    forin_prefix_prepare(traces,&fixture,spec->backend,spec->scenario);forin_prefix_observe(&traces[0],&witness);
    XrXirInstanceResult event={.outcome={.status=XR_XIR_CALL_READY}};
    for(unsigned prefix=0;prefix<spec->prefix;++prefix) {
        CHECK(!forin_terminal(event));if(event.outcome.status==XR_XIR_CALL_SUSPENDED)forin_prefix_resume(traces,0,event,spec->scenario);
        event=forin_tick(traces,0);forin_prefix_observe(&traces[0],&witness);
    }
    unsigned registered=0,entered=0;
    for(unsigned i=0;i<witness.count;++i){registered+=(unsigned)witness.producers[i].registered;entered+=(unsigned)witness.producers[i].entered;}
    unsigned committed[8],outputs=traces[0].output_count;memcpy(committed,traces[0].output,sizeof(committed));
    unsigned pending=registered-forin_output_count(&traces[0],701),root_pending=witness.root_registered-forin_output_count(&traces[0],810);
    CHECK(pending<=2 && root_pending<=1);bool terminal=forin_terminal(event);
    unsigned root_mask=traces[0].root_mask;
    if(terminal)forin_prefix_terminal_witness(&traces[0],event,spec->scenario);
    if(terminal && spec->mode==2 && event.outcome.status==XR_XIR_CALL_RETURNED)
        CHECK(xr_xir_value_copy(&event.outcome.value,&traces[0].value)==XR_XIR_VALUE_OK);
    unsigned status=forin_prefix_control(traces,spec,event);
    CHECK(traces[0].output_count==outputs+pending+root_pending && !memcmp(traces[0].output,committed,outputs*sizeof(*committed)));
    for(unsigned i=0;i<pending;++i)CHECK(traces[0].output[outputs+i]==701);
    if(root_pending)CHECK(traces[0].output[outputs+pending]==810);
    if(terminal && traces[0].instance && event.outcome.status==XR_XIR_CALL_RETURNED) {
        CHECK(xr_xir_instance_take_result(traces[0].instance,&traces[0].value)==XR_XIR_CALL_RETURNED);
        if(spec->scenario==1)CHECK(traces[0].value.type==XR_XIR_BOOL && traces[0].value.payload==1);
        else iterator_text(&traces[0].value,forin_expected[spec->scenario]);
    }
    XrXirInstanceResult peer={.outcome={.status=XR_XIR_CALL_READY}};
    do {
        if(peer.outcome.status==XR_XIR_CALL_SUSPENDED)forin_prefix_resume(traces,1,peer,spec->scenario);
        peer=forin_tick(traces,1);CHECK(traces[1].polls<=forin_measured_polls[spec->scenario]);
    }while(!forin_terminal(peer));
    forin_finish(&traces[1],peer,spec->scenario);CHECK(traces[1].polls==forin_measured_polls[spec->scenario]);
    forin_prefix_retire(traces,&fixture,spec);
    CHECK(traces[0].output_count==outputs+pending+root_pending);
    printf("For-in prefix backend=%u scenario=%u mode=%u prefix=%u close=%u drop=%u rootCodeMask=%u rootCurrentEntered=%u rootCurrentFrontier=%u rootDeferEntered=%u rootRegistered=%u entered=%u registered=%u committed=%u pending=%u rootPending=%u terminal=%u control=%u sites=%zu value0=%llu call0=%llu work0=%llu value1=%llu call1=%llu work1=%llu healthyPeerComplete actualFrontiers committedPrefix stickyTerminal originalDomains2 finalReferenceZero2 codeLease1 noAllocateDrop physical0/0\n",spec->backend,spec->scenario,spec->mode,spec->prefix,spec->close,spec->drop,root_mask,witness.current_root_entered,witness.current_root_frontier,witness.root_entered,witness.root_registered,entered,registered,outputs,pending,root_pending,(unsigned)terminal,status,runtime_attempts,(unsigned long long)forin_final_cost[0].requested_bytes,(unsigned long long)forin_final_cost[0].requested_call_bytes,(unsigned long long)forin_final_cost[0].work,(unsigned long long)forin_final_cost[1].requested_bytes,(unsigned long long)forin_final_cost[1].requested_call_bytes,(unsigned long long)forin_final_cost[1].work);
}
static unsigned forin_prefix_number(const char *text) {
    char *end=NULL;unsigned long n=strtoul(text,&end,10);CHECK(text[0] && end && !*end && n<20000);return(unsigned)n;
}
int main(int argc,char **argv) {
    CHECK(argc==8);(void)forin_prefix_original_main;const char *names[]={"0","1","vm","native"};unsigned backend=0;
    while(backend<4 && strcmp(argv[1],names[backend]))++backend;CHECK(backend<4);
    ForinPrefixSpec spec={backend,forin_prefix_number(argv[3]),forin_prefix_number(argv[4]),forin_prefix_number(argv[5]),forin_prefix_number(argv[6]),forin_prefix_number(argv[7])};
    CHECK(spec.scenario<12 && spec.mode<3 && spec.close<2 && spec.drop<2 && spec.prefix<=forin_measured_polls[spec.scenario]);
    if(!strcmp(argv[2],"one")){forin_prefix_operation(&spec);return 0;}
    CHECK(!strcmp(argv[2],"suite") && spec.prefix==forin_measured_polls[spec.scenario]);
    for(unsigned prefix=0;prefix<=spec.prefix;++prefix){ForinPrefixSpec current=spec;current.prefix=prefix;forin_prefix_operation(&current);}
    return 0;
}
