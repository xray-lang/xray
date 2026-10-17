/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_mixed_forin_lifetime.c - Lexical loop exits own authentic producers and late values
 */
#include "xir_mixed_forin_fixture.h"

typedef struct ForinTrace {
    IteratorFixture *fixture;
    XrXirInstance *instance;
    XrXirValue iterator,alias,value;
    uintptr_t cells[8];
    unsigned cell_count,root_mask,producer_mask,polls,timers,wait_kinds,foreign_wait_refusals;
    unsigned output[8],output_count;
    bool done;
} ForinTrace;
static const unsigned forin_timer_count[12]={1,2,1,2,1,1,2,2,2,2,2,2};
static const unsigned forin_timers[12][2]={{5,0},{5,7},{5,0},{5,7},{5,0},{5,0},{5,5},{5,7},{5,7},{5,7},{5,7},{5,7}};
static const char *const forin_expected[12]={"cedar",NULL,"cedar","cedarbirch","caught",NULL,"cedar","cedar","cedarbirch","generic","thrown","unit"};
static XrXirOutputStatus forin_output(void *context,const XrXirOutputGroup *group) {
    ForinTrace *trace=context;
    CHECK(trace->instance && group && group->line && group->stream==XR_XIR_STDOUT && group->count==1);
    CHECK(group->values[0].type==XR_XIR_I64 && trace->output_count<8);
    trace->output[trace->output_count++]=(unsigned)group->values[0].payload;
    return XR_XIR_OUTPUT_OK;
}
static void forin_frames(ForinTrace *trace,XrXirCall *call) {
    if(!call)return;
    CHECK(!call->driving && call->config.instance==trace->instance && call->budget==&trace->instance->budget);
    CHECK(call->config.admission.domain==trace->instance->domain);
    for(const CallFrame *frame=call->top;frame;frame=frame->parent) {
        uintptr_t base=(uintptr_t)call->config.entries,address=(uintptr_t)frame->entry;
        CHECK(address>=base && (address-base)%sizeof(*frame->entry)==0);
        uint64_t index=(address-base)/sizeof(*frame->entry);CHECK(index<call->config.entry_count);
        if(index>=trace->fixture->count)continue;
        bool root=trace->fixture->root[index],native=iterator_native(trace->fixture->backend,root);
        const XrXirCallEntry *entry=native ? &p20_iterator_lifetime_fixture_program.entries[index] : &trace->fixture->vm->entries[index];
        CHECK(frame->entry->resume==entry->resume && frame->entry->environment==entry->environment);
        if(frame->entered) {
            CHECK(frame->code_kind==(native ? XR_XIR_CALL_CODE_NATIVE : XR_XIR_CALL_CODE_VM));
            if(root)trace->root_mask|=native ? 2u : 1u;
        }else CHECK(!frame->code_kind);
        if(!(entry->flags&XR_XIR_ENTRY_GENERATOR))continue;
        CHECK(!root && entry->parameter_count==1 && frame->arguments);
        const XrXirValue *cell=&frame->arguments[0];CHECK(xr_xir_cell_in_domain(cell,trace->instance->domain));
        const XirCell *object=(const XirCell *)(uintptr_t)cell->payload;
        CHECK(object->initialized && !object->module_storage && !object->module_owner && iterator_physical((uintptr_t)object));
        bool seen=false;for(unsigned i=0;i<trace->cell_count;++i)seen|=trace->cells[i]==(uintptr_t)object;
        if(!seen){CHECK(trace->cell_count<8);trace->cells[trace->cell_count++]=(uintptr_t)object;}
        if(frame->entered)trace->producer_mask|=native ? 2u : 1u;
    }
}
static XrXirInstanceResult forin_tick(ForinTrace traces[2],unsigned at) {
    ForinTrace *trace=&traces[at];XrXirDomainBudgetStats before={0};
    if(traces[1-at].instance)before=xr_xir_domain_budget_stats(traces[1-at].instance->domain);
    XrXirInstanceResult event=xr_xir_instance_poll_bounded(trace->instance,1);++trace->polls;
    forin_frames(trace,trace->instance->call);
    for(XirGenerator *g=trace->instance->executor->generators;g;g=g->active_next) {
        CHECK(g->executor==trace->instance->executor && g->object.domain==trace->instance->domain);
        forin_frames(trace,g->call);
    }
    if(traces[1-at].instance){XrXirDomainBudgetStats after=xr_xir_domain_budget_stats(traces[1-at].instance->domain);
        CHECK(before.requested_bytes==after.requested_bytes && before.requested_call_bytes==after.requested_call_bytes && before.work==after.work);}
    forin_costs();
    return event;
}
static void forin_make(ForinTrace traces[2],unsigned at) {
    ForinTrace *trace=&traces[at];CHECK(xr_xir_instance_start(trace->instance,p20_iterator_lifetime_entries[0],NULL,0)==XR_XIR_CALL_READY);
    XrXirInstanceResult event={0};unsigned count=0;
    do{event=forin_tick(traces,at);CHECK(++count<4096);}while(event.outcome.status==XR_XIR_CALL_READY);
    CHECK(event.outcome.status==XR_XIR_CALL_RETURNED && !trace->output_count);
    CHECK(xr_xir_instance_take_result(trace->instance,&trace->iterator)==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_value_valid(&trace->iterator) && xr_xir_value_copy(&trace->iterator,&trace->alias)==XR_XIR_VALUE_OK);
    XirGenerator *g=(XirGenerator *)(uintptr_t)trace->iterator.payload;
    CHECK(g->object.kind==XR_XIR_TYPE_ITERATOR && g->object.domain==trace->instance->domain && !g->consumer && !g->accounting.polls);
    trace->root_mask=trace->producer_mask=trace->polls=0;
}
static void forin_resume(ForinTrace traces[2],unsigned at,XrXirInstanceResult event,unsigned scenario) {
    ForinTrace *trace=&traces[at];XrXirWaitRequest request={0};
    CHECK(event.epoch && event.outcome.wake && xr_xir_instance_wait_request(trace->instance,event.epoch,event.outcome.wake,&request)==XR_XIR_CALL_READY);
    CHECK(trace->timers<forin_timer_count[scenario] && request.kind==XR_XIR_WAIT_TIMER_MS && !request.reserved);
    CHECK(request.after_ms==forin_timers[scenario][trace->timers++]);
    unsigned refusals=trace->foreign_wait_refusals;
    for(XirGenerator *g=trace->instance->executor->generators;g;g=g->active_next)if(g->consumer) {
        XrXirTaskWaitToken token={0};if(!xr_xir_call_iterator_wait_token(g->consumer,&token))continue;
        CHECK(token.request.subject==g && token.request.kind>=XR_XIR_WAIT_ITERATOR_HAS_NEXT && token.request.kind<=XR_XIR_WAIT_ITERATOR_CLOSE);
        trace->wait_kinds|=1u<<token.request.kind;
        CHECK(xr_xir_call_complete_iterator_wait(traces[1-at].instance->call,&token)==XR_XIR_CALL_BAD_STATE);++trace->foreign_wait_refusals;
    }
    CHECK(trace->foreign_wait_refusals>refusals);
    CHECK(xr_xir_instance_resume(trace->instance,event.epoch+1,event.outcome.wake)==XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_instance_resume(trace->instance,event.epoch,event.outcome.wake+1)==XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_instance_resume(trace->instance,event.epoch,event.outcome.wake)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_resume(trace->instance,event.epoch,event.outcome.wake)==XR_XIR_CALL_BAD_STATE);
}
static void forin_finish(ForinTrace *trace,XrXirInstanceResult event,unsigned scenario) {
    if(scenario==5) {
        CHECK(event.outcome.status==XR_XIR_CALL_BOUNDS && event.outcome.panic.detail.code==XR_XIR_PANIC_BOUNDS);
        CHECK(xr_xir_instance_take_result(trace->instance,&trace->value)==XR_XIR_CALL_BAD_STATE && !trace->value.type && !trace->value.payload);
    }else {
        if(event.outcome.status!=XR_XIR_CALL_RETURNED)fprintf(stderr,"For-in scenario%u terminal%u\n",scenario,event.outcome.status);
        CHECK(event.outcome.status==XR_XIR_CALL_RETURNED && xr_xir_instance_take_result(trace->instance,&trace->value)==XR_XIR_CALL_RETURNED);
        if(scenario==1)CHECK(trace->value.type==XR_XIR_BOOL && trace->value.payload==1);else iterator_text(&trace->value,forin_expected[scenario]);
    }
    const unsigned ordinary[]={701,scenario==0 ? 801u : scenario==1 ? 802u : 810u};
    const unsigned nested[]={701,820,701,810},alias[]={701,820,810};
    const unsigned *expected=scenario==6 ? nested : scenario==7 ? alias : ordinary;unsigned count=scenario==6 ? 4 : scenario==7 ? 3 : 2;
    if(trace->output_count!=count || memcmp(trace->output,expected,count*sizeof(*expected))) {
        fprintf(stderr,"For-in scenario%u outputs",scenario);for(unsigned i=0;i<trace->output_count;++i)fprintf(stderr," %u",trace->output[i]);fputc('\n',stderr);
    }
    CHECK(trace->output_count==count && !memcmp(trace->output,expected,count*sizeof(*expected)));
    CHECK(trace->timers==forin_timer_count[scenario] && trace->root_mask==(iterator_native(trace->fixture->backend,true) ? 2u : 1u));
    CHECK(trace->producer_mask==(iterator_native(trace->fixture->backend,false) ? 2u : 1u) && trace->cell_count);
    const XirGenerator *g=(const XirGenerator *)(uintptr_t)trace->iterator.payload;
    CHECK(!g->consumer);
    if(scenario<10)CHECK(!g->call);else CHECK(!g->accounting.polls);
    trace->done=true;
}
static void forin_graph(unsigned backend,unsigned scenario,unsigned close,unsigned drop) {
    CHECK(!runtime_live && !runtime_bytes && !effects_compile_live && !forin_borrowed_domain[0] && !forin_borrowed_domain[1]);runtime_fail_at=SIZE_MAX;runtime_attempts=0;
    memset(forin_final_cost,0,sizeof(forin_final_cost));memset(forin_previous_cost,0,sizeof(forin_previous_cost));memset(forin_domain_disposals,0,sizeof(forin_domain_disposals));
    IteratorFixture fixture={0};XrXirProgram *program=iterator_program(&fixture,backend);ForinTrace traces[2]={{0}};
    for(unsigned i=0;i<2;++i) {
        traces[i].fixture=&fixture;XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
        config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,forin_output,&traces[i]};CHECK(xr_xir_instance_new(program,&config,&traces[i].instance)==XR_XIR_CALL_READY);
        forin_borrowed_domain[i]=traces[i].instance->domain;forin_costs();
    }
    CHECK(traces[0].instance->domain!=traces[1].instance->domain);xr_xir_compile_program_drop(program);CHECK(!fixture.releases);
    for(unsigned i=0;i<2;++i)forin_make(traces,i);
    CHECK(traces[0].iterator.payload!=traces[1].iterator.payload);
    for(unsigned i=0;i<2;++i) {
        XrXirValue foreign[]={traces[1-i].iterator,{XR_XIR_I64,0,0}};uint64_t epoch=traces[i].instance->epoch;size_t attempts=runtime_attempts;
        CHECK(xr_xir_instance_start(traces[i].instance,p20_iterator_lifetime_entries[3],foreign,2)==XR_XIR_CALL_BAD_ARGUMENT);
        CHECK(traces[i].instance->epoch==epoch && runtime_attempts==attempts && !traces[i].output_count);
        XrXirValue arguments[]={traces[i].iterator,{XR_XIR_I64,0,scenario>=2 ? scenario-2 : 0}};unsigned operation=scenario<2 ? scenario+1 : 3;
        CHECK(xr_xir_instance_start(traces[i].instance,p20_iterator_lifetime_entries[operation],arguments,operation==3 ? 2 : 1)==XR_XIR_CALL_READY);
    }
    for(unsigned turns=0;!traces[0].done || !traces[1].done;++turns) {
        CHECK(turns<4096);
        for(unsigned i=0;i<2;++i)if(!traces[i].done) {
            XrXirInstanceResult event=forin_tick(traces,i);
            if(event.outcome.status==XR_XIR_CALL_SUSPENDED)forin_resume(traces,i,event,scenario);
            else if(event.outcome.status!=XR_XIR_CALL_READY)forin_finish(&traces[i],event,scenario);
        }
    }
    for(unsigned a=0;a<traces[0].cell_count;++a)for(unsigned b=0;b<traces[1].cell_count;++b)CHECK(traces[0].cells[a]!=traces[1].cells[b]);
    unsigned outputs[]={traces[0].output_count,traces[1].output_count};
    for(unsigned n=0;n<2;++n){unsigned at=n ? 1-close : close;CHECK(xr_xir_instance_free(traces[at].instance)==XR_XIR_CALL_READY);traces[at].instance=NULL;CHECK(traces[at].output_count==outputs[at]);}
    CHECK(fixture.releases==1 && !fixture.vm && !fixture.entries);size_t attempts=runtime_attempts;
    for(unsigned n=0;n<2;++n) {
        unsigned at=n ? 1-drop : drop;uintptr_t address=(uintptr_t)traces[at].iterator.payload;
        const XirGenerator *g=(const XirGenerator *)address;CHECK(xr_xir_value_valid(&traces[at].iterator) && !g->executor && !g->consumer && !g->call);
        xr_xir_value_drop(&traces[at].iterator);CHECK(iterator_physical(address));xr_xir_value_drop(&traces[at].alias);CHECK(!iterator_physical(address));
    }
    forin_costs();CHECK(attempts==runtime_attempts && !effects_compile_live && !effects_compile_bytes);
    for(unsigned i=0;i<2;++i) {if(forin_expected[scenario])iterator_text(&traces[i].value,forin_expected[scenario]);xr_xir_value_drop(&traces[i].value);}
    CHECK(!runtime_live && !runtime_bytes && !forin_borrowed_domain[0] && !forin_borrowed_domain[1] && forin_domain_disposals[0]==1 && forin_domain_disposals[1]==1);
    CHECK(runtime_attempts && runtime_attempts<20000);
    printf("P20 for-in backend=%u scenario=%u close=%u drop=%u timers=%u/%u outputs=%u/%u cells=%u/%u polls=%u/%u foreignWaits=%u/%u waitKinds=%u/%u authenticFrames foreignWaitRefusal peerLedgerUnchanged codeLease1 noAllocateDrop lateStringAfterMetadata originalDomains2 finalReferenceZero2 sites=%zu value0=%llu call0=%llu work0=%llu value1=%llu call1=%llu work1=%llu physical0/0\n",backend,scenario,close,drop,traces[0].timers,traces[1].timers,traces[0].output_count,traces[1].output_count,traces[0].cell_count,traces[1].cell_count,traces[0].polls,traces[1].polls,traces[0].foreign_wait_refusals,traces[1].foreign_wait_refusals,traces[0].wait_kinds,traces[1].wait_kinds,runtime_attempts,(unsigned long long)forin_final_cost[0].requested_bytes,(unsigned long long)forin_final_cost[0].requested_call_bytes,(unsigned long long)forin_final_cost[0].work,(unsigned long long)forin_final_cost[1].requested_bytes,(unsigned long long)forin_final_cost[1].requested_call_bytes,(unsigned long long)forin_final_cost[1].work);
}
int main(int argc,char **argv) {
    CHECK(argc==2);const char *names[]={"0","1","vm","native"};unsigned backend=0;
    while(backend<4 && strcmp(argv[1],names[backend]))++backend;CHECK(backend<4);
    for(unsigned scenario=0;scenario<12;++scenario)for(unsigned close=0;close<2;++close)for(unsigned drop=0;drop<2;++drop)forin_graph(backend,scenario,close,drop);
    return 0;
}
