/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_mixed_atomic_control.h - Actual Source call prefixes and owned values after control
 */
#ifndef XIR_MIXED_ATOMIC_CONTROL_H
#define XIR_MIXED_ATOMIC_CONTROL_H
#include "xir_mixed_atomic_resources.h"
typedef struct AtomicControlSpec {unsigned backend,victim,call,entry,prefix,polls,mode,close,drop;} AtomicControlSpec;
typedef struct AtomicControlRun {
    AtomicResourceRun resources;
    AtomicControlSpec spec;
    XrXirValue task,expected;
    XrXirCallResult root_before,root_after;
    unsigned phase,control_status,root_after_status,drain_polls,late_atomics,late_tasks;
    bool applied,cut[2],preterminal,task_preterminal,root_after_available;
} AtomicControlRun;
static XrXirValue atomic_control_expected(unsigned entry,unsigned phase) {
    if(entry<6 || entry==10 || entry==11) {
        uint64_t value=entry<2 || entry==10?(phase?47u:40u):entry<4 || entry==11?(phase==2?30u:phase?3u:0u):41u;
        return atomic_resource_operand(XR_XIR_I64,value);
    }
    if(entry==6 || entry==7 || entry==12)return atomic_resource_operand(XR_XIR_F64,atomic_f64_bits(entry==6?40.5:phase?2.5:1.5));
    if(entry==8 || entry==9 || entry==13)return atomic_resource_operand(XR_XIR_BOOL,entry==8 || phase);
    CHECK(entry==14);return atomic_resource_operand(XR_XIR_UNIT,0);
}
static void atomic_control_scalar(XrXirValue value,XrXirValue expected) {
    atomic_scalar(&value,(XrXirType)expected.type,(uint64_t)expected.payload);
}
static void atomic_control_root_value(AtomicControlRun *c,const XrXirValue *value) {
    if(c->spec.entry<10 || c->spec.entry==14){atomic_control_scalar(*value,c->expected);return;}
    CHECK(xr_xir_value_valid(value));const XirObject *object=(const XirObject *)(uintptr_t)value->payload;
    CHECK(object->kind==XR_XIR_TYPE_ATOMIC && object->domain==atomic_domains[c->spec.victim]);
    atomic_check_operation(value,XR_XIR_ATOMIC_OPERATION_LOAD,(XrXirType)c->expected.type,0,(uint64_t)c->expected.payload);++c->late_atomics;
}
/* Only an actual stopped TASK wait publishes a Task value suitable for copying. */
static void atomic_control_capture(AtomicControlRun *c) {
    XrXirInstance *instance=c->resources.graph.instances[c->spec.victim];XrXirCall *call=instance->call;XrXirTaskWaitToken token={0};
    c->phase=instance->executor->active?1u:0u;
    if(!call || !call->wait_linked || !call->waiting_task.type || !xr_xir_call_task_wait_token(call,&token))return;
    const XirTask *task=(const XirTask *)(uintptr_t)call->waiting_task.payload;
    CHECK(xr_xir_task_storage_valid(task) && task->object.domain==atomic_domains[c->spec.victim]);
    CHECK(token.binding.owner==instance->executor && token.binding.activation==&instance->executor->root);
    CHECK(token.binding.generation==instance->executor->generation && token.binding.ticket==instance->executor->root.ticket);
    CHECK(token.request.kind==XR_XIR_WAIT_TASK && token.request.subject==task);
    CHECK(xr_xir_value_copy(&call->waiting_task,&c->task)==XR_XIR_VALUE_OK && c->task.payload==call->waiting_task.payload);
    c->task_preterminal=task->state==XIR_TASK_TERMINAL;c->phase=c->task_preterminal?3u:2u;
    if(c->task_preterminal)CHECK(task->outcome.status==XR_XIR_CALL_RETURNED && !task->call && !task->executor);
    else {
        CHECK(task->executor==instance->executor && task->call && task->activation.call==task->call && task->activation.task==task);
        XrXirCallResult empty={0};size_t attempts=runtime_attempts;XrXirDomainBudgetStats before=xr_xir_domain_budget_stats(task->object.domain);
        CHECK(xr_xir_task_copy_outcome(&c->task,&empty)==XR_XIR_CALL_BAD_STATE && xr_xir_call_result_empty(&empty));
        XrXirDomainBudgetStats after=xr_xir_domain_budget_stats(task->object.domain);
        CHECK(runtime_attempts==attempts && before.requested_bytes==after.requested_bytes && before.requested_call_bytes==after.requested_call_bytes && before.work==after.work);
    }
}
static XrXirInstanceResult atomic_control_tick(AtomicControlRun *c,unsigned owner) {
    AtomicResourceRun *r=&c->resources;XrXirDomainBudgetStats peer=atomic_resource_peer(owner);atomic_observe(&r->graph,owner);
    XrXirInstanceResult event=xr_xir_instance_poll_bounded(r->graph.instances[owner],1);++r->graph.trace[owner].polls;
    atomic_observe(&r->graph,owner);atomic_resource_costs();atomic_resource_peer_equal(owner,peer);
    CHECK(xr_xir_call_result_valid(&event.outcome));return event;
}
static void atomic_control_action(AtomicControlRun *c,XrXirInstanceResult before) {
    CHECK(!c->applied && !c->cut[c->spec.victim]);AtomicResourceRun *r=&c->resources;unsigned owner=c->spec.victim;
    CHECK(r->calls[owner]==c->spec.call+1 && r->call_polls[owner][c->spec.call]==c->spec.prefix);
    c->applied=true;c->expected=atomic_control_expected(c->spec.entry,r->graph.phase[owner]);
    c->preterminal=before.outcome.status==XR_XIR_CALL_RETURNED;
    CHECK(before.outcome.status==XR_XIR_CALL_READY || c->preterminal);
    CHECK(c->preterminal==(c->spec.prefix==c->spec.polls));atomic_control_capture(c);
    if(c->preterminal) {
        CHECK(!before.outcome.wake && xr_xir_panic_empty(&before.outcome.panic));
        if(c->spec.entry<10 || c->spec.entry==14)atomic_control_scalar(before.outcome.value,c->expected);
        CHECK(xr_xir_call_result_copy(&before.outcome,&c->root_before)==XR_XIR_VALUE_OK);
    }
    XrXirDomainBudgetStats peer=atomic_resource_peer(owner);
    if(!c->spec.mode) {
        c->control_status=(unsigned)xr_xir_instance_cancel_current(r->graph.instances[owner]);
        CHECK(c->control_status==(unsigned)(c->preterminal?XR_XIR_CALL_BAD_STATE:XR_XIR_CALL_CANCEL_REQUESTED));
    }else if(c->spec.mode==1) {
        c->control_status=(unsigned)xr_xir_instance_stop(r->graph.instances[owner]);CHECK(c->control_status==XR_XIR_CALL_READY);
    }else {
        CHECK(c->spec.mode==2);c->control_status=(unsigned)xr_xir_instance_free(r->graph.instances[owner]);r->graph.instances[owner]=NULL;
        CHECK(c->control_status==XR_XIR_CALL_READY);
    }
    atomic_resource_costs();atomic_resource_peer_equal(owner,peer);
    if(c->spec.mode<2) {
        XrXirInstanceResult after={0};
        do{after=atomic_control_tick(c,owner);CHECK(++c->drain_polls<4096);}while(after.outcome.status==XR_XIR_CALL_READY);
        CHECK(after.outcome.status==(c->preterminal?XR_XIR_CALL_RETURNED:XR_XIR_CALL_CANCELLED));
        CHECK(!after.outcome.wake && xr_xir_panic_empty(&after.outcome.panic));
        CHECK(xr_xir_call_result_copy(&after.outcome,&c->root_after)==XR_XIR_VALUE_OK);c->root_after_available=true;c->root_after_status=(unsigned)after.outcome.status;
        if(!c->preterminal)CHECK(!after.outcome.value.type && !after.outcome.value.reserved && !after.outcome.value.payload);
        else CHECK(after.outcome.value.type==c->root_before.value.type && after.outcome.value.payload==c->root_before.value.payload);
        for(unsigned sticky=0;sticky<2;++sticky) {
            XrXirInstanceResult same=atomic_control_tick(c,owner);
            CHECK(same.outcome.status==after.outcome.status && same.outcome.value.type==after.outcome.value.type && same.outcome.value.payload==after.outcome.value.payload && !same.outcome.wake && xr_xir_panic_empty(&same.outcome.panic));
        }
    }
    c->cut[owner]=true;atomic_resource_costs();atomic_resource_peer_equal(owner,peer);
}
static void atomic_control_pair(AtomicControlRun *c,unsigned entry,unsigned mask,XrXirValue output[2]) {
    AtomicResourceRun *r=&c->resources;CHECK(entry<ATOMIC_RUNTIME_ENTRIES);r->entry=entry;++r->step;bool done[2]={true,true};unsigned current[2]={0};
    for(unsigned i=0;i<2;++i)if((mask&(1u<<i)) && !c->cut[i]) {
        CHECK(r->graph.instances[i] && r->calls[i]<40);current[i]=r->calls[i]++;r->call_entries[i][current[i]]=entry;
        XrXirDomainBudgetStats peer=atomic_resource_peer(i);CHECK(xr_xir_instance_start(r->graph.instances[i],p20_atomic_runtime_entries[entry],NULL,0)==XR_XIR_CALL_READY);
        atomic_resource_costs();atomic_resource_peer_equal(i,peer);done[i]=false;
        if(c->spec.mode<3 && i==c->spec.victim && current[i]==c->spec.call) {
            CHECK(entry==c->spec.entry);if(!c->spec.prefix){atomic_control_action(c,(XrXirInstanceResult){0});done[i]=true;}
        }
    }
    unsigned total=0;
    while(!done[0] || !done[1])for(unsigned i=0;i<2;++i)if(!done[i]) {
        XrXirInstanceResult event=atomic_control_tick(c,i);++r->call_polls[i][current[i]];CHECK(++total<8192);
        CHECK(event.outcome.status==XR_XIR_CALL_READY || event.outcome.status==XR_XIR_CALL_RETURNED);
        if(c->spec.mode<3 && i==c->spec.victim && current[i]==c->spec.call && r->call_polls[i][current[i]]==c->spec.prefix) {
            atomic_control_action(c,event);done[i]=true;continue;
        }
        if(event.outcome.status==XR_XIR_CALL_RETURNED) {
            CHECK(xr_xir_instance_take_result(r->graph.instances[i],&output[i])==XR_XIR_CALL_RETURNED);
            XrXirValue empty={0};CHECK(xr_xir_instance_take_result(r->graph.instances[i],&empty)==XR_XIR_CALL_BAD_STATE && !empty.type && !empty.payload);done[i]=true;
        }
    }
}
static void atomic_control_round(AtomicControlRun *c) {
    for(unsigned entry=0;entry<10;++entry) {
        XrXirValue out[2]={{0},{0}};atomic_control_pair(c,entry,3,out);
        for(unsigned i=0;i<2;++i) {
            if(c->cut[i]){CHECK(!out[i].type && !out[i].payload);continue;}
            atomic_control_scalar(out[i],atomic_control_expected(entry,c->resources.graph.phase[i]));xr_xir_value_drop(&out[i]);
        }
    }
}
static void atomic_control_holds(AtomicControlRun *c) {
    AtomicResourceRun *r=&c->resources;
    for(unsigned field=0;field<4;++field) {
        XrXirValue values[2]={{0},{0}};atomic_control_pair(c,10+field,3,values);
        for(unsigned i=0;i<2;++i)if(!c->cut[i]) {
            const XirObject *object=(const XirObject *)(uintptr_t)values[i].payload;
            CHECK(xr_xir_value_valid(&values[i]) && object->kind==XR_XIR_TYPE_ATOMIC && object->domain==atomic_domains[i]);r->graph.held[i][field]=values[i];
        }
        if(values[0].type && values[1].type)CHECK(values[0].payload!=values[1].payload);
    }
    for(unsigned i=0;i<2;++i)if(!c->cut[i]) {
        XrXirValue alias={0};CHECK(xr_xir_value_copy(&r->graph.held[i][0],&alias)==XR_XIR_VALUE_OK);
        CHECK(alias.type==r->graph.held[i][0].type && alias.payload==r->graph.held[i][0].payload);xr_xir_value_drop(&alias);
    }
}
static void atomic_control_change(AtomicControlRun *c) {
    AtomicResourceRun *r=&c->resources;
    if(!c->cut[0]) {
        CHECK(atomic_resource_operation(r,0,&r->graph.held[0][0],XR_XIR_ATOMIC_OPERATION_FETCH_ADD,atomic_resource_operand(XR_XIR_I64,7),40));
        CHECK(atomic_resource_operation(r,0,&r->graph.held[0][1],XR_XIR_ATOMIC_OPERATION_FETCH_ADD,atomic_resource_operand(XR_XIR_I64,3),0));
        CHECK(atomic_resource_operation(r,0,&r->graph.held[0][2],XR_XIR_ATOMIC_OPERATION_SWAP,atomic_resource_operand(XR_XIR_F64,atomic_f64_bits(2.5)),atomic_f64_bits(1.5)));
        CHECK(atomic_resource_operation(r,0,&r->graph.held[0][3],XR_XIR_ATOMIC_OPERATION_SWAP,atomic_resource_operand(XR_XIR_BOOL,1),0));r->graph.phase[0]=1;
    }
    atomic_control_round(c);XrXirValue unit[2]={{0},{0}};atomic_control_pair(c,14,1,unit);
    if(!c->cut[0]){atomic_control_scalar(unit[0],atomic_resource_operand(XR_XIR_UNIT,0));xr_xir_value_drop(&unit[0]);r->graph.phase[0]=2;}
    XrXirValue current[2]={{0},{0}};atomic_control_pair(c,11,3,current);
    if(current[0].type){CHECK(current[0].payload!=r->graph.held[0][1].payload);r->graph.held[0][4]=current[0];}
    xr_xir_value_drop(&current[1]);atomic_control_round(c);
    if(!c->cut[0]) {
        CHECK(atomic_resource_operation(r,0,&r->graph.held[0][1],XR_XIR_ATOMIC_OPERATION_LOAD,atomic_resource_operand(XR_XIR_I64,0),3));
        CHECK(atomic_resource_operation(r,0,&r->graph.held[0][4],XR_XIR_ATOMIC_OPERATION_LOAD,atomic_resource_operand(XR_XIR_I64,0),30));
    }
}
static void atomic_control_late_task(AtomicControlRun *c) {
    if(!c->task.type)return;const XirTask *task=(const XirTask *)(uintptr_t)c->task.payload;
    CHECK(task->state==XIR_TASK_TERMINAL && !task->call && !task->executor);uintptr_t address=(uintptr_t)task;
    XrXirCallStatus expected=c->task_preterminal || !c->spec.mode?XR_XIR_CALL_RETURNED:XR_XIR_CALL_CANCELLED;
    XrXirCallResult outcomes[2]={{0},{0}};
    for(unsigned copy=0;copy<2;++copy) {
        CHECK(xr_xir_task_copy_outcome(&c->task,&outcomes[copy])==expected && !outcomes[copy].wake && xr_xir_panic_empty(&outcomes[copy].panic));
        if(expected==XR_XIR_CALL_RETURNED)atomic_control_scalar(outcomes[copy].value,c->expected);
        else CHECK(!outcomes[copy].value.type && !outcomes[copy].value.reserved && !outcomes[copy].value.payload);
    }
    size_t attempts=runtime_attempts;xr_xir_value_drop(&c->task);CHECK(!atomic_runtime_physical(address) && runtime_attempts==attempts);
    for(unsigned copy=0;copy<2;++copy)xr_xir_call_result_drop(&outcomes[copy]);c->late_tasks=1;
}
static void atomic_control_retire(AtomicControlRun *c) {
    AtomicResourceRun *r=&c->resources;
    for(unsigned n=0;n<2;++n) {
        unsigned i=n?1-c->spec.close:c->spec.close;if(!r->graph.instances[i])continue;
        XrXirDomainBudgetStats peer=atomic_resource_peer(i);CHECK(xr_xir_instance_free(r->graph.instances[i])==XR_XIR_CALL_READY);r->graph.instances[i]=NULL;
        atomic_resource_costs();atomic_resource_peer_equal(i,peer);
    }
    CHECK(r->graph.fixture.releases==1 && !r->graph.fixture.vm && !r->graph.fixture.entries);
    if(c->preterminal)atomic_control_root_value(c,&c->root_before.value);
    if(c->root_after_available && c->root_after.status==XR_XIR_CALL_RETURNED)atomic_control_root_value(c,&c->root_after.value);
    atomic_control_late_task(c);
    for(unsigned i=0;i<2;++i) {
        unsigned phase=r->graph.phase[i];
        for(unsigned field=0;field<5;++field)if(r->graph.held[i][field].type) {
            XrXirType type=field==2?XR_XIR_F64:field==3?XR_XIR_BOOL:XR_XIR_I64;
            uint64_t expected=field==0?(phase?47u:40u):field==1?(phase?3u:0u):field==2?atomic_f64_bits(phase?2.5:1.5):field==3?(uint64_t)(phase!=0):30u;
            XrXirAtomicOperation operation=field==3?XR_XIR_ATOMIC_OPERATION_SWAP:XR_XIR_ATOMIC_OPERATION_LOAD;
            CHECK(atomic_resource_operation(r,i,&r->graph.held[i][field],operation,atomic_resource_operand(type,field==3?!phase:0),expected));++c->late_atomics;
        }
    }
    size_t attempts=runtime_attempts;xr_xir_call_result_drop(&c->root_before);xr_xir_call_result_drop(&c->root_after);
    for(unsigned n=0;n<2;++n) {
        unsigned i=n?1-c->spec.drop:c->spec.drop;
        for(unsigned field=0;field<5;++field)if(r->graph.held[i][field].type) {
            uintptr_t address=(uintptr_t)r->graph.held[i][field].payload;CHECK(atomic_runtime_physical(address));
            xr_xir_value_drop(&r->graph.held[i][field]);CHECK(!atomic_runtime_physical(address));
        }
        CHECK(!atomic_domains[i] && atomic_domain_disposals[i]==1);
    }
    CHECK(runtime_attempts==attempts && !runtime_live && !runtime_bytes);effects_source_owners_free();
}
static AtomicResourceResult atomic_control_transaction(const AtomicControlSpec *spec) {
    CHECK(spec->backend<4 && spec->victim<2 && spec->call<(spec->victim?35u:36u) && spec->entry<15 && spec->prefix<=spec->polls && spec->mode<4 && spec->close<2 && spec->drop<2);
    CHECK(!effects_compile_live && !runtime_live && !atomic_domains[0] && !atomic_domains[1]);
    memset(atomic_final_cost,0,sizeof(atomic_final_cost));memset(atomic_previous_cost,0,sizeof(atomic_previous_cost));memset(atomic_domain_disposals,0,sizeof(atomic_domain_disposals));
    runtime_fail_at=SIZE_MAX;runtime_attempts=0;effects_compile_attempts=0;AtomicControlRun c={.spec=*spec};AtomicResourceRun *r=&c.resources;
    XrXirProgram *program=NULL;CHECK(atomic_runtime_prepare(&r->graph.fixture,spec->backend,false,&program) && program && !runtime_live);
    for(unsigned i=0;i<2;++i) {
        CHECK(xr_xir_instance_config_init(&atomic_resource_configs[i],sizeof(atomic_resource_configs[i]))==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_new(program,&atomic_resource_configs[i],&r->graph.instances[i])==XR_XIR_CALL_READY);atomic_domains[i]=r->graph.instances[i]->domain;
    }
    CHECK(atomic_domains[0]!=atomic_domains[1]);xr_xir_compile_program_drop(program);
    atomic_control_round(&c);atomic_control_holds(&c);atomic_control_change(&c);
    if(spec->mode<3)CHECK(c.applied && c.cut[spec->victim] && !c.cut[1-spec->victim] && r->calls[spec->victim]==spec->call+1);
    else CHECK(!c.applied && !c.cut[0] && !c.cut[1]);
    for(unsigned i=0;i<2;++i)if(!c.cut[i]) {
        CHECK(r->graph.trace[i].tasks==30 && r->graph.trace[i].polls==(i?543u:555u) && r->calls[i]==(i?35u:36u));
        CHECK(r->graph.trace[i].root_mask==(atomic_runtime_native(spec->backend,true)?2u:1u) && r->graph.trace[i].worker_mask==(atomic_runtime_native(spec->backend,false)?2u:1u));
    }
    atomic_control_retire(&c);CHECK(!effects_compile_live && !effects_compile_bytes && !runtime_live && !runtime_bytes && runtime_attempts<20000);
    AtomicResourceResult result={.sites=runtime_attempts,.complete=true};
    for(unsigned i=0;i<2;++i) {
        result.cost[i]=atomic_final_cost[i];result.unaffected[i]=!c.cut[i];result.calls[i]=r->calls[i];result.polls[i]=r->graph.trace[i].polls;result.tasks[i]=r->graph.trace[i].tasks;
    }
    if(spec->mode==3) {
        CHECK(runtime_attempts==437 && c.late_atomics==9);
        printf("Atomic control baseline backend=%u close=%u drop=%u sites=%zu calls=%u/%u polls=%u/%u tasks=%u/%u lateAtomics=%u value0=%llu call0=%llu work0=%llu value1=%llu call1=%llu work1=%llu extendedLateNew30Load originalDomains2 codeLease1 physical0/0\n",
            spec->backend,spec->close,spec->drop,runtime_attempts,r->calls[0],r->calls[1],r->graph.trace[0].polls,r->graph.trace[1].polls,r->graph.trace[0].tasks,r->graph.trace[1].tasks,c.late_atomics,(unsigned long long)atomic_final_cost[0].requested_bytes,(unsigned long long)atomic_final_cost[0].requested_call_bytes,(unsigned long long)atomic_final_cost[0].work,(unsigned long long)atomic_final_cost[1].requested_bytes,(unsigned long long)atomic_final_cost[1].requested_call_bytes,(unsigned long long)atomic_final_cost[1].work);fflush(stdout);return result;
    }
    unsigned peer=1-spec->victim;
    printf("Atomic control backend=%u victim=%u call=%u entry=%u prefix=%u of%u mode=%u close=%u drop=%u preterminal=%u phase=%u capturedTask=%u taskPreterminal=%u control=%u rootAfterAvailable=%u rootAfter=%u drainPolls=%u lateAtomics=%u lateTasks=%u peerCalls=%u peerPolls=%u peerTasks=%u sites=%zu value0=%llu call0=%llu work0=%llu value1=%llu call1=%llu work1=%llu realPrefix originalDomains2 lastReferenceZero2 codeLease1 lateValuesAfterCodeDeath noAllocateDrop physical0/0\n",
        spec->backend,spec->victim,spec->call,spec->entry,spec->prefix,spec->polls,spec->mode,spec->close,spec->drop,(unsigned)c.preterminal,c.phase,(unsigned)(c.phase>=2),(unsigned)c.task_preterminal,c.control_status,(unsigned)c.root_after_available,c.root_after_status,c.drain_polls,c.late_atomics,c.late_tasks,r->calls[peer],r->graph.trace[peer].polls,r->graph.trace[peer].tasks,runtime_attempts,(unsigned long long)atomic_final_cost[0].requested_bytes,(unsigned long long)atomic_final_cost[0].requested_call_bytes,(unsigned long long)atomic_final_cost[0].work,(unsigned long long)atomic_final_cost[1].requested_bytes,(unsigned long long)atomic_final_cost[1].requested_call_bytes,(unsigned long long)atomic_final_cost[1].work);fflush(stdout);return result;
}
#endif // XIR_MIXED_ATOMIC_CONTROL_H
