/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_mixed_atomic_control_resources.h - Accepted faults across actual Atomic control graphs
 */
#ifndef XIR_MIXED_ATOMIC_CONTROL_RESOURCES_H
#define XIR_MIXED_ATOMIC_CONTROL_RESOURCES_H
#include "xir_mixed_atomic_control.h"
typedef struct AtomicBudgetRun {
    AtomicControlRun control;
    bool selected,root_before_owned,root_after_owned,task_execution_proven;
    unsigned stage;
} AtomicBudgetRun;
typedef struct AtomicBudgetResult {
    AtomicResourceResult resource;
    unsigned selected,control_attempted,control_completed,phase,late_atomics,late_tasks;
} AtomicBudgetResult;
static bool atomic_budget_running(const AtomicBudgetRun *b,unsigned owner) {
    return !b->control.cut[owner] && !b->control.resources.failed[owner] && b->control.resources.graph.instances[owner];
}
static void atomic_budget_call_failure(AtomicBudgetRun *b,unsigned owner,const XrXirCall *call,const XirTaskActivation *activation) {
    if(!call)return;AtomicResourceRun *r=&b->control.resources;const XrXirTaskExecutor *executor=r->graph.instances[owner]->executor;
    CHECK(activation->call==call);XrXirExecutorBinding binding={executor,(void *)activation,executor->generation,activation->ticket};
    XrXirCallStatus status=XR_XIR_CALL_READY;CHECK(xr_xir_call_driver_failure(call,&binding,&status));
    if(status==r->expected_fault)atomic_resource_fault(r,owner);else CHECK(status==XR_XIR_CALL_READY || status==XR_XIR_CALL_CANCELLED);
}
static XrXirInstanceResult atomic_budget_tick(AtomicBudgetRun *b,unsigned owner) {
    AtomicResourceRun *r=&b->control.resources;XrXirDomainBudgetStats peer=atomic_resource_peer(owner);atomic_observe(&r->graph,owner);
    XrXirInstanceResult event=xr_xir_instance_poll_bounded(r->graph.instances[owner],1);++r->graph.trace[owner].polls;
    const XrXirTaskExecutor *executor=r->graph.instances[owner]->executor;
    atomic_budget_call_failure(b,owner,executor->root.call,&executor->root);
    for(const XirTask *task=executor->active;task;task=task->active_next)atomic_budget_call_failure(b,owner,task->call,&task->activation);
    if(executor->first_failure==r->expected_fault || event.outcome.status==r->expected_fault)atomic_resource_fault(r,owner);
    else CHECK(executor->first_failure==XR_XIR_CALL_READY || executor->first_failure==XR_XIR_CALL_CANCELLED);
    CHECK(event.outcome.status==r->expected_fault || event.outcome.status==XR_XIR_CALL_READY || event.outcome.status==XR_XIR_CALL_RETURNED || event.outcome.status==XR_XIR_CALL_CANCELLED);
    CHECK(xr_xir_call_result_valid(&event.outcome));
    if(!r->failed[owner])atomic_observe(&r->graph,owner);
    atomic_resource_costs();atomic_resource_peer_equal(owner,peer);return event;
}
static bool atomic_budget_capture(AtomicBudgetRun *b) {
    AtomicControlRun *c=&b->control;AtomicResourceRun *r=&c->resources;unsigned owner=c->spec.victim;
    XrXirInstance *instance=r->graph.instances[owner];XrXirCall *call=instance->call;XrXirTaskWaitToken token={0};
    c->phase=instance->executor->active?1u:0u;
    if(!call || !call->wait_linked || !call->waiting_task.type || !xr_xir_call_task_wait_token(call,&token))return true;
    const XirTask *task=(const XirTask *)(uintptr_t)call->waiting_task.payload;
    CHECK(xr_xir_task_storage_valid(task) && task->object.domain==atomic_domains[owner]);
    CHECK(token.binding.owner==instance->executor && token.binding.activation==&instance->executor->root);
    CHECK(token.binding.generation==instance->executor->generation && token.binding.ticket==instance->executor->root.ticket);
    CHECK(token.request.kind==XR_XIR_WAIT_TASK && token.request.subject==task);
    if(!atomic_resource_value_status(r,owner,xr_xir_value_copy(&call->waiting_task,&c->task))){CHECK(!c->task.type && !c->task.payload);return false;}
    CHECK(c->task.payload==call->waiting_task.payload);c->task_preterminal=task->state==XIR_TASK_TERMINAL;c->phase=c->task_preterminal?3u:2u;
    if(c->task_preterminal)CHECK(task->outcome.status==XR_XIR_CALL_RETURNED && !task->call && !task->executor);
    else {
        CHECK(task->executor==instance->executor && task->call && task->activation.call==task->call && task->activation.task==task);
        XrXirCallResult empty={0};size_t attempts=runtime_attempts;XrXirDomainBudgetStats before=xr_xir_domain_budget_stats(task->object.domain);
        CHECK(xr_xir_task_copy_outcome(&c->task,&empty)==XR_XIR_CALL_BAD_STATE && xr_xir_call_result_empty(&empty));
        XrXirDomainBudgetStats after=xr_xir_domain_budget_stats(task->object.domain);
        CHECK(runtime_attempts==attempts && before.requested_bytes==after.requested_bytes && before.requested_call_bytes==after.requested_call_bytes && before.work==after.work);
    }
    return true;
}
static void atomic_budget_action(AtomicBudgetRun *b,XrXirInstanceResult before) {
    AtomicControlRun *c=&b->control;AtomicResourceRun *r=&c->resources;unsigned owner=c->spec.victim;
    CHECK(!b->selected && !c->applied && !r->failed[owner]);b->selected=true;b->stage=2;
    CHECK(r->calls[owner]==c->spec.call+1 && r->call_polls[owner][c->spec.call]==c->spec.prefix);
    c->expected=atomic_control_expected(c->spec.entry,r->graph.phase[owner]);c->preterminal=before.outcome.status==XR_XIR_CALL_RETURNED;
    CHECK(before.outcome.status==XR_XIR_CALL_READY || c->preterminal);CHECK(c->preterminal==(c->spec.prefix==c->spec.polls));
    if(!atomic_budget_capture(b))return;
    if(c->preterminal) {
        CHECK(!before.outcome.wake && xr_xir_panic_empty(&before.outcome.panic));
        if(c->spec.entry<10 || c->spec.entry==14)atomic_control_scalar(before.outcome.value,c->expected);
        ++r->step;r->entry=22;
        if(!atomic_resource_value_status(r,owner,xr_xir_call_result_copy(&before.outcome,&c->root_before))){CHECK(xr_xir_call_result_empty(&c->root_before));return;}
        b->root_before_owned=true;
    }
    XrXirDomainBudgetStats peer=atomic_resource_peer(owner);c->applied=true;++r->step;r->entry=18+c->spec.mode;
    XrXirCallStatus actual,expected;
    if(!c->spec.mode) {actual=xr_xir_instance_cancel_current(r->graph.instances[owner]);expected=c->preterminal?XR_XIR_CALL_BAD_STATE:XR_XIR_CALL_CANCEL_REQUESTED;}
    else if(c->spec.mode==1){actual=xr_xir_instance_stop(r->graph.instances[owner]);expected=XR_XIR_CALL_READY;}
    else {CHECK(c->spec.mode==2);actual=xr_xir_instance_free(r->graph.instances[owner]);r->graph.instances[owner]=NULL;expected=XR_XIR_CALL_READY;}
    c->control_status=(unsigned)actual;c->cut[owner]=true;
    bool success=atomic_resource_status(r,owner,actual,expected);atomic_resource_costs();atomic_resource_peer_equal(owner,peer);
    if(!success)return;
    if(c->spec.mode==2)b->task_execution_proven=true;
    if(c->spec.mode<2) {
        XrXirInstanceResult after={0};
        do{after=atomic_budget_tick(b,owner);CHECK(++c->drain_polls<4096);if(r->failed[owner])return;}while(after.outcome.status==XR_XIR_CALL_READY);
        CHECK(after.outcome.status==(c->preterminal?XR_XIR_CALL_RETURNED:XR_XIR_CALL_CANCELLED));
        CHECK(!after.outcome.wake && xr_xir_panic_empty(&after.outcome.panic));b->task_execution_proven=true;
        c->root_after_available=true;c->root_after_status=(unsigned)after.outcome.status;
        ++r->step;r->entry=23;
        if(!atomic_resource_value_status(r,owner,xr_xir_call_result_copy(&after.outcome,&c->root_after))){CHECK(xr_xir_call_result_empty(&c->root_after));return;}
        b->root_after_owned=true;
        if(!c->preterminal)CHECK(!after.outcome.value.type && !after.outcome.value.reserved && !after.outcome.value.payload);
        else CHECK(after.outcome.value.type==c->root_before.value.type && after.outcome.value.payload==c->root_before.value.payload);
        for(unsigned sticky=0;sticky<2;++sticky) {
            XrXirInstanceResult same=atomic_budget_tick(b,owner);if(r->failed[owner])return;
            CHECK(same.outcome.status==after.outcome.status && same.outcome.value.type==after.outcome.value.type && same.outcome.value.payload==after.outcome.value.payload && !same.outcome.wake && xr_xir_panic_empty(&same.outcome.panic));
        }
    }
    atomic_resource_costs();atomic_resource_peer_equal(owner,peer);
}
static void atomic_budget_pair(AtomicBudgetRun *b,unsigned entry,unsigned mask,XrXirValue output[2]) {
    AtomicControlRun *c=&b->control;AtomicResourceRun *r=&c->resources;r->entry=entry;++r->step;b->stage=1;
    bool done[2]={true,true};unsigned current[2]={0};
    for(unsigned i=0;i<2;++i)if((mask&(1u<<i)) && atomic_budget_running(b,i)) {
        CHECK(r->calls[i]<40);current[i]=r->calls[i]++;r->call_entries[i][current[i]]=entry;
        XrXirDomainBudgetStats peer=atomic_resource_peer(i);
        bool started=atomic_resource_status(r,i,xr_xir_instance_start(r->graph.instances[i],p20_atomic_runtime_entries[entry],NULL,0),XR_XIR_CALL_READY);
        atomic_resource_costs();atomic_resource_peer_equal(i,peer);done[i]=!started;
        if(started && i==c->spec.victim && current[i]==c->spec.call) {
            CHECK(entry==c->spec.entry);if(!c->spec.prefix){atomic_budget_action(b,(XrXirInstanceResult){0});done[i]=true;}
        }
    }
    unsigned total=0;
    while(!done[0] || !done[1])for(unsigned i=0;i<2;++i)if(!done[i]) {
        XrXirInstanceResult event=atomic_budget_tick(b,i);++r->call_polls[i][current[i]];CHECK(++total<8192);
        if(r->failed[i]){done[i]=true;CHECK(!output[i].type && !output[i].payload);continue;}
        CHECK(event.outcome.status==XR_XIR_CALL_READY || event.outcome.status==XR_XIR_CALL_RETURNED);
        if(i==c->spec.victim && current[i]==c->spec.call && r->call_polls[i][current[i]]==c->spec.prefix){atomic_budget_action(b,event);done[i]=true;continue;}
        if(event.outcome.status==XR_XIR_CALL_RETURNED) {
            if(!atomic_resource_status(r,i,xr_xir_instance_take_result(r->graph.instances[i],&output[i]),XR_XIR_CALL_RETURNED))CHECK(!output[i].type && !output[i].payload);
            else {XrXirValue empty={0};CHECK(xr_xir_instance_take_result(r->graph.instances[i],&empty)==XR_XIR_CALL_BAD_STATE && !empty.type && !empty.payload);}
            done[i]=true;
        }
    }
}
static void atomic_budget_round(AtomicBudgetRun *b) {
    for(unsigned entry=0;entry<10;++entry) {
        XrXirValue out[2]={{0},{0}};atomic_budget_pair(b,entry,3,out);
        for(unsigned i=0;i<2;++i) {
            if(!atomic_budget_running(b,i)){CHECK(!out[i].type && !out[i].payload);continue;}
            atomic_control_scalar(out[i],atomic_control_expected(entry,b->control.resources.graph.phase[i]));xr_xir_value_drop(&out[i]);
        }
    }
}
static void atomic_budget_holds(AtomicBudgetRun *b) {
    AtomicResourceRun *r=&b->control.resources;
    for(unsigned field=0;field<4;++field) {
        XrXirValue values[2]={{0},{0}};atomic_budget_pair(b,10+field,3,values);
        for(unsigned i=0;i<2;++i)if(atomic_budget_running(b,i)) {
            const XirObject *object=(const XirObject *)(uintptr_t)values[i].payload;
            CHECK(xr_xir_value_valid(&values[i]) && object->kind==XR_XIR_TYPE_ATOMIC && object->domain==atomic_domains[i]);r->graph.held[i][field]=values[i];
        }
        if(values[0].type && values[1].type)CHECK(values[0].payload!=values[1].payload);
    }
    for(unsigned i=0;i<2;++i)if(atomic_budget_running(b,i)) {
        XrXirValue alias={0};
        if(atomic_resource_value_status(r,i,xr_xir_value_copy(&r->graph.held[i][0],&alias))){CHECK(alias.type==r->graph.held[i][0].type && alias.payload==r->graph.held[i][0].payload);xr_xir_value_drop(&alias);}
        else CHECK(!alias.type && !alias.payload);
    }
}
static void atomic_budget_change(AtomicBudgetRun *b) {
    AtomicResourceRun *r=&b->control.resources;
    if(atomic_budget_running(b,0)) {
        bool changed=atomic_resource_operation(r,0,&r->graph.held[0][0],XR_XIR_ATOMIC_OPERATION_FETCH_ADD,atomic_resource_operand(XR_XIR_I64,7),40);
        changed=atomic_resource_operation(r,0,&r->graph.held[0][1],XR_XIR_ATOMIC_OPERATION_FETCH_ADD,atomic_resource_operand(XR_XIR_I64,3),0) && changed;
        changed=atomic_resource_operation(r,0,&r->graph.held[0][2],XR_XIR_ATOMIC_OPERATION_SWAP,atomic_resource_operand(XR_XIR_F64,atomic_f64_bits(2.5)),atomic_f64_bits(1.5)) && changed;
        changed=atomic_resource_operation(r,0,&r->graph.held[0][3],XR_XIR_ATOMIC_OPERATION_SWAP,atomic_resource_operand(XR_XIR_BOOL,1),0) && changed;
        if(changed)r->graph.phase[0]=1;
    }
    atomic_budget_round(b);XrXirValue unit[2]={{0},{0}};atomic_budget_pair(b,14,1,unit);
    if(atomic_budget_running(b,0)){atomic_control_scalar(unit[0],atomic_resource_operand(XR_XIR_UNIT,0));xr_xir_value_drop(&unit[0]);r->graph.phase[0]=2;}
    XrXirValue current[2]={{0},{0}};atomic_budget_pair(b,11,3,current);
    if(current[0].type){CHECK(current[0].payload!=r->graph.held[0][1].payload);r->graph.held[0][4]=current[0];}
    xr_xir_value_drop(&current[1]);atomic_budget_round(b);
    if(atomic_budget_running(b,0)) {
        (void)atomic_resource_operation(r,0,&r->graph.held[0][1],XR_XIR_ATOMIC_OPERATION_LOAD,atomic_resource_operand(XR_XIR_I64,0),3);
        (void)atomic_resource_operation(r,0,&r->graph.held[0][4],XR_XIR_ATOMIC_OPERATION_LOAD,atomic_resource_operand(XR_XIR_I64,0),30);
    }
    for(unsigned i=0;i<2;++i)r->finished[i]=!r->failed[i] && (i==b->control.spec.victim?b->control.applied:atomic_budget_running(b,i));
}
static void atomic_budget_root_value(AtomicBudgetRun *b,const XrXirValue *value) {
    AtomicControlRun *c=&b->control;
    if(c->spec.entry<10 || c->spec.entry==14){atomic_control_scalar(*value,c->expected);return;}
    CHECK(xr_xir_value_valid(value));const XirObject *object=(const XirObject *)(uintptr_t)value->payload;
    CHECK(object->kind==XR_XIR_TYPE_ATOMIC && object->domain==atomic_domains[c->spec.victim]);
    if(atomic_resource_operation(&c->resources,c->spec.victim,value,XR_XIR_ATOMIC_OPERATION_LOAD,atomic_resource_operand((XrXirType)c->expected.type,0),(uint64_t)c->expected.payload))++c->late_atomics;
}
static void atomic_budget_late_task(AtomicBudgetRun *b) {
    AtomicControlRun *c=&b->control;AtomicResourceRun *r=&c->resources;if(!c->task.type)return;
    const XirTask *task=(const XirTask *)(uintptr_t)c->task.payload;CHECK(task->state==XIR_TASK_TERMINAL && !task->call && !task->executor);
    XrXirCallStatus stored=task->outcome.status,expected=c->task_preterminal || !c->spec.mode?XR_XIR_CALL_RETURNED:XR_XIR_CALL_CANCELLED;
    if(c->task_preterminal || b->task_execution_proven)CHECK(stored==expected);
    else CHECK(stored==XR_XIR_CALL_RETURNED || stored==XR_XIR_CALL_CANCELLED || stored==r->expected_fault);
    if(stored==XR_XIR_CALL_RETURNED)atomic_control_scalar(task->outcome.value,c->expected);
    uintptr_t address=(uintptr_t)task;XrXirCallResult outcomes[2]={{0},{0}};
    for(unsigned copy=0;copy<2;++copy) {
        ++r->step;r->entry=21;XrXirCallStatus status=xr_xir_task_copy_outcome(&c->task,&outcomes[copy]);
        if(xr_xir_call_result_empty(&outcomes[copy])) {
            CHECK(status==r->expected_fault && xr_xir_value_valid(&c->task) && task->outcome.status==stored);atomic_resource_fault(r,c->spec.victim);
        }else {
            CHECK(status==stored && !outcomes[copy].wake && xr_xir_panic_empty(&outcomes[copy].panic));
            if(stored==XR_XIR_CALL_RETURNED)atomic_control_scalar(outcomes[copy].value,c->expected);
            else CHECK(!outcomes[copy].value.type && !outcomes[copy].value.reserved && !outcomes[copy].value.payload);
        }
    }
    size_t attempts=runtime_attempts;xr_xir_value_drop(&c->task);CHECK(!atomic_runtime_physical(address) && runtime_attempts==attempts);
    for(unsigned copy=0;copy<2;++copy)xr_xir_call_result_drop(&outcomes[copy]);c->late_tasks=1;
}
static void atomic_budget_retire(AtomicBudgetRun *b) {
    AtomicControlRun *c=&b->control;AtomicResourceRun *r=&c->resources;b->stage=3;
    for(unsigned n=0;n<2;++n) {
        unsigned i=n?1-c->spec.close:c->spec.close;if(!r->graph.instances[i])continue;
        ++r->step;r->entry=20;XrXirDomainBudgetStats peer=atomic_resource_peer(i);XrXirCallStatus status=xr_xir_instance_free(r->graph.instances[i]);r->graph.instances[i]=NULL;
        (void)atomic_resource_status(r,i,status,XR_XIR_CALL_READY);atomic_resource_costs();atomic_resource_peer_equal(i,peer);
    }
    CHECK(r->graph.fixture.releases==1 && !r->graph.fixture.vm && !r->graph.fixture.entries);b->stage=4;
    if(b->root_before_owned)atomic_budget_root_value(b,&c->root_before.value);
    if(b->root_after_owned && c->root_after.status==XR_XIR_CALL_RETURNED)atomic_budget_root_value(b,&c->root_after.value);
    atomic_budget_late_task(b);
    for(unsigned i=0;i<2;++i) {
        unsigned phase=r->graph.phase[i];
        for(unsigned field=0;field<5;++field)if(r->graph.held[i][field].type) {
            CHECK(xr_xir_value_valid(&r->graph.held[i][field]));XrXirType type=field==2?XR_XIR_F64:field==3?XR_XIR_BOOL:XR_XIR_I64;
            uint64_t expected=field==0?(phase?47u:40u):field==1?(phase?3u:0u):field==2?atomic_f64_bits(phase?2.5:1.5):field==3?(uint64_t)(phase!=0):30u;
            XrXirAtomicOperation operation=field==3?XR_XIR_ATOMIC_OPERATION_SWAP:XR_XIR_ATOMIC_OPERATION_LOAD;
            if(atomic_resource_operation(r,i,&r->graph.held[i][field],operation,atomic_resource_operand(type,field==3?!phase:0),expected))++c->late_atomics;
        }
    }
    size_t attempts=runtime_attempts;xr_xir_call_result_drop(&c->root_before);xr_xir_call_result_drop(&c->root_after);
    for(unsigned n=0;n<2;++n) {
        unsigned i=n?1-c->spec.drop:c->spec.drop;
        for(unsigned field=0;field<5;++field)if(r->graph.held[i][field].type) {
            uintptr_t address=(uintptr_t)r->graph.held[i][field].payload;CHECK(atomic_runtime_physical(address));xr_xir_value_drop(&r->graph.held[i][field]);CHECK(!atomic_runtime_physical(address));
        }
        CHECK(!atomic_domains[i] && atomic_domain_disposals[i]==(unsigned)((r->published_mask&(1u<<i))!=0));
    }
    CHECK(runtime_attempts==attempts && !runtime_live && !runtime_bytes);effects_source_owners_free();
}
static AtomicBudgetResult atomic_budget_transaction(const AtomicControlSpec *spec,const XrXirInstanceConfig *caps,unsigned limited_owner,size_t failure) {
    CHECK(spec->backend<4 && spec->victim<2 && spec->call<(spec->victim?35u:36u) && spec->entry<15 && spec->prefix<=spec->polls && spec->mode<3 && spec->close<2 && spec->drop<2 && limited_owner<2);
    CHECK(!effects_compile_live && !runtime_live && !atomic_domains[0] && !atomic_domains[1]);
    memset(atomic_final_cost,0,sizeof(atomic_final_cost));memset(atomic_previous_cost,0,sizeof(atomic_previous_cost));memset(atomic_domain_disposals,0,sizeof(atomic_domain_disposals));
    runtime_fail_at=SIZE_MAX;runtime_attempts=0;effects_compile_attempts=0;AtomicBudgetRun b={0};b.control.spec=*spec;
    AtomicControlRun *c=&b.control;AtomicResourceRun *r=&c->resources;r->expected_fault=caps?XR_XIR_CALL_LIMIT:XR_XIR_CALL_OOM;r->fault_owner=2;
    XrXirProgram *program=NULL;CHECK(atomic_runtime_prepare(&r->graph.fixture,spec->backend,false,&program) && program && !runtime_live);
    for(unsigned i=0;i<2;++i)CHECK(xr_xir_instance_config_init(&atomic_resource_configs[i],sizeof(atomic_resource_configs[i]))==XR_XIR_CALL_READY);
    if(caps)atomic_resource_configs[limited_owner]=*caps;
    runtime_attempts=0;runtime_fail_at=failure;
    for(unsigned i=0;i<2;++i) {
        ++r->step;r->entry=17;
        if(!atomic_resource_status(r,i,xr_xir_instance_new(program,&atomic_resource_configs[i],&r->graph.instances[i]),XR_XIR_CALL_READY)){CHECK(!r->graph.instances[i]);continue;}
        atomic_domains[i]=r->graph.instances[i]->domain;r->published_mask|=1u<<i;atomic_resource_costs();
    }
    if(r->published_mask==3)CHECK(atomic_domains[0]!=atomic_domains[1]);xr_xir_compile_program_drop(program);
    atomic_budget_round(&b);atomic_budget_holds(&b);atomic_budget_change(&b);
    for(unsigned i=0;i<2;++i)if(!r->failed[i]) {
        if(i==spec->victim)CHECK(c->applied && c->cut[i] && r->calls[i]==spec->call+1);
        else CHECK(r->graph.trace[i].tasks==30 && r->graph.trace[i].polls==(i?543u:555u) && r->calls[i]==(i?35u:36u));
    }
    atomic_budget_retire(&b);AtomicResourceResult result={0};
    for(unsigned i=0;i<2;++i) {
        result.cost[i]=atomic_final_cost[i];result.fault_cost[i]=r->fault_cost[i];result.unaffected[i]=r->finished[i] && !r->failed[i];result.disposals+=atomic_domain_disposals[i];
        result.calls[i]=r->calls[i];result.polls[i]=r->graph.trace[i].polls;result.tasks[i]=r->graph.trace[i].tasks;result.root_mask[i]=r->graph.trace[i].root_mask;result.worker_mask[i]=r->graph.trace[i].worker_mask;
    }
    result.complete=!r->accepted_fault;result.fault=r->accepted_fault;result.fault_owner=r->fault_owner;result.fault_step=r->fault_step;result.fault_entry=r->fault_entry;result.fault_observed=r->fault_observed;result.published_mask=r->published_mask;
    result.sites=runtime_attempts;result.hit=failure!=SIZE_MAX && runtime_attempts>failure;runtime_fail_at=SIZE_MAX;
    CHECK(!effects_compile_live && !effects_compile_bytes && !runtime_live && !runtime_bytes && result.sites<20000);
    return (AtomicBudgetResult){result,(unsigned)b.selected,(unsigned)c->applied,(unsigned)b.task_execution_proven,c->phase,c->late_atomics,c->late_tasks};
}
static AtomicResourceResult atomic_budget_baseline(const AtomicControlSpec *spec) {
    AtomicResourceResult expected=atomic_control_transaction(spec);AtomicBudgetResult observed=atomic_budget_transaction(spec,NULL,0,SIZE_MAX);AtomicResourceResult actual=observed.resource;
    CHECK(observed.selected && observed.control_attempted && observed.control_completed);
    CHECK(actual.complete && actual.published_mask==3 && actual.disposals==2 && actual.sites==expected.sites);
    for(unsigned i=0;i<2;++i) {
        CHECK(actual.unaffected[i] && actual.cost[i].requested_bytes==expected.cost[i].requested_bytes && actual.cost[i].requested_call_bytes==expected.cost[i].requested_call_bytes && actual.cost[i].work==expected.cost[i].work);
        CHECK(actual.calls[i]==expected.calls[i] && actual.polls[i]==expected.polls[i] && actual.tasks[i]==expected.tasks[i]);
    }
    printf("Atomic control resource normal backend=%u victim=%u call=%u entry=%u prefix=%u of%u mode=%u close=%u drop=%u sites=%zu value0=%llu call0=%llu work0=%llu value1=%llu call1=%llu work1=%llu sameOriginalControlGraph originalDomains2 codeLease1 physical0/0\n",
        spec->backend,spec->victim,spec->call,spec->entry,spec->prefix,spec->polls,spec->mode,spec->close,spec->drop,actual.sites,(unsigned long long)actual.cost[0].requested_bytes,(unsigned long long)actual.cost[0].requested_call_bytes,(unsigned long long)actual.cost[0].work,(unsigned long long)actual.cost[1].requested_bytes,(unsigned long long)actual.cost[1].requested_call_bytes,(unsigned long long)actual.cost[1].work);fflush(stdout);return actual;
}
static void atomic_budget_axes(const AtomicControlSpec *spec,const AtomicResourceResult *normal) {
    for(unsigned owner=0;owner<2;++owner) {
        uint64_t fees[]={normal->cost[owner].requested_bytes,normal->cost[owner].requested_call_bytes,normal->cost[owner].work};
        for(unsigned axis=0;axis<3;++axis)for(unsigned minus=0;minus<2;++minus) {
            CHECK(fees[axis]>1);XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
            uint64_t cap=fees[axis]-minus;if(!axis)config.requested_value_limit=cap;else if(axis==1)config.requested_call_limit=cap;else config.work_limit=cap;
            AtomicBudgetResult observed=atomic_budget_transaction(spec,&config,owner,SIZE_MAX);AtomicResourceResult r=observed.resource;
            printf("Atomic control resource axis backend=%u victim=%u call=%u entry=%u prefix=%u of%u mode=%u close=%u drop=%u owner=%u axis=%u minus=%u cap=%llu complete=%u fault=%u faultOwner=%u faultStep=%u faultEntry=%u faultObserved=%u publishedMask=%u selected=%u controlAttempted=%u controlCompleted=%u unaffected=%u/%u value0=%llu call0=%llu work0=%llu value1=%llu call1=%llu work1=%llu physical0/0\n",
                spec->backend,spec->victim,spec->call,spec->entry,spec->prefix,spec->polls,spec->mode,spec->close,spec->drop,owner,axis,minus,(unsigned long long)cap,(unsigned)r.complete,r.fault,r.fault_owner,r.fault_step,r.fault_entry,r.fault_observed,r.published_mask,observed.selected,observed.control_attempted,observed.control_completed,(unsigned)r.unaffected[0],(unsigned)r.unaffected[1],(unsigned long long)r.cost[0].requested_bytes,(unsigned long long)r.cost[0].requested_call_bytes,(unsigned long long)r.cost[0].work,(unsigned long long)r.cost[1].requested_bytes,(unsigned long long)r.cost[1].requested_call_bytes,(unsigned long long)r.cost[1].work);fflush(stdout);
            CHECK(r.complete==!minus && r.fault==(unsigned)(minus?XR_XIR_CALL_LIMIT:XR_XIR_CALL_READY) && r.unaffected[1-owner]);
            if(minus)CHECK(r.fault_owner==owner);
            for(unsigned i=0;i<2;++i)if(!minus || i!=owner)CHECK(r.cost[i].requested_bytes==normal->cost[i].requested_bytes && r.cost[i].requested_call_bytes==normal->cost[i].requested_call_bytes && r.cost[i].work==normal->cost[i].work);
        }
    }
}
#endif // XIR_MIXED_ATOMIC_CONTROL_RESOURCES_H
