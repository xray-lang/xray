/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_mixed_atomic_resources.h - Original Domain Atomic transactions with accepted failures
 */
#ifndef XIR_MIXED_ATOMIC_RESOURCES_H
#define XIR_MIXED_ATOMIC_RESOURCES_H
#include "xir/xxir_vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if(!(c)){fprintf(stderr,"Atomic runtime %d: %s\n",__LINE__,#c);exit(1);} } while(0)
#include "xir_effect_execution_owner.h"
static void atomic_resource_before_free(void *pointer);
/* The allocator calls this observer on the real block before physical free. */
#define XIR_RUNTIME_BEFORE_FREE(pointer) atomic_resource_before_free(pointer)
#include "xir_runtime_allocations.h"
#undef XIR_RUNTIME_BEFORE_FREE
#define main atomic_resource_original_main
#include "test_xir_mixed_atomic_lifetime.c"
#undef main

static XrXirInstanceConfig atomic_resource_configs[2];
static void atomic_resource_cost(unsigned owner,XrXirDomainBudgetStats cost) {
    CHECK(owner<2 && cost.bound);const XrXirInstanceConfig *config=&atomic_resource_configs[owner];
    CHECK(cost.requested_limit==config->requested_value_limit && cost.requested_call_limit==config->requested_call_limit && cost.work_limit==config->work_limit);
    CHECK(cost.requested_bytes>=atomic_previous_cost[owner].requested_bytes && cost.requested_call_bytes>=atomic_previous_cost[owner].requested_call_bytes && cost.work>=atomic_previous_cost[owner].work);
    CHECK(cost.requested_bytes<=cost.requested_limit && cost.requested_call_bytes<=cost.requested_call_limit && cost.work<=cost.work_limit);
    atomic_previous_cost[owner]=cost;
}
static void atomic_resource_costs(void) {
    for(unsigned i=0;i<2;++i)if(atomic_domains[i])atomic_resource_cost(i,xr_xir_domain_budget_stats(atomic_domains[i]));
}
static void atomic_resource_before_free(void *pointer) {
    for(unsigned i=0;pointer && i<2;++i)if(pointer==atomic_domains[i]) {
        XrXirDomain *domain=pointer;CHECK(!atomic_load_explicit(&domain->references,memory_order_relaxed) && !domain->objects);
        CHECK(domain->stats.live_bytes==sizeof(*domain) && domain->stats.allocations==domain->stats.frees+1);
        CHECK(!domain->budget.call_live && !domain->budget.metadata_live);
        CHECK(domain->budget.call_allocations==domain->budget.call_frees && domain->budget.metadata_allocations==domain->budget.metadata_frees);
        atomic_resource_cost(i,domain->budget);atomic_final_cost[i]=domain->budget;CHECK(!atomic_domain_disposals[i]++);atomic_domains[i]=NULL;
    }
}
typedef struct AtomicResourceResult {
    XrXirDomainBudgetStats cost[2],fault_cost[2];
    unsigned polls[2],tasks[2],root_mask[2],worker_mask[2];
    unsigned calls[2],call_entries[2][40],call_polls[2][40];
    unsigned fault,fault_owner,fault_step,fault_entry,fault_observed,published_mask,disposals;
    size_t sites;
    bool complete,unaffected[2],hit;
} AtomicResourceResult;
typedef struct AtomicResourceRun {
    AtomicGraph graph;
    XrXirDomainBudgetStats fault_cost[2];
    XrXirCallStatus expected_fault,accepted_fault;
    unsigned fault_owner,fault_step,fault_entry,fault_observed,step,entry,published_mask;
    unsigned calls[2],call_entries[2][40],call_polls[2][40];
    bool failed[2],finished[2];
} AtomicResourceRun;
static void atomic_resource_fault(AtomicResourceRun *r,unsigned owner) {
    CHECK(owner<2);r->failed[owner]=true;
    if(r->accepted_fault!=XR_XIR_CALL_READY)return;
    r->accepted_fault=r->expected_fault;r->fault_owner=owner;r->fault_step=r->step;r->fault_entry=r->entry;
    for(unsigned i=0;i<2;++i) {
        if(atomic_domains[i]){r->fault_cost[i]=xr_xir_domain_budget_stats(atomic_domains[i]);r->fault_observed|=1u<<i;}
        else if(atomic_domain_disposals[i]){r->fault_cost[i]=atomic_final_cost[i];r->fault_observed|=1u<<i;}
    }
}
static bool atomic_resource_status(AtomicResourceRun *r,unsigned owner,XrXirCallStatus status,XrXirCallStatus expected) {
    if(status==r->expected_fault){atomic_resource_fault(r,owner);return false;}
    if(status!=expected)fprintf(stderr,"Atomic resource step=%u entry=%u owner=%u actual=%u expected=%u fault=%u\n",r->step,r->entry,owner,(unsigned)status,(unsigned)expected,(unsigned)r->expected_fault);
    CHECK(status==expected);return true;
}
static bool atomic_resource_value_status(AtomicResourceRun *r,unsigned owner,XrXirValueStatus status) {
    if(status==XR_XIR_VALUE_OK)return true;
    CHECK(status==(r->expected_fault==XR_XIR_CALL_LIMIT?XR_XIR_VALUE_LIMIT:XR_XIR_VALUE_OOM));
    atomic_resource_fault(r,owner);return false;
}
static XrXirDomainBudgetStats atomic_resource_peer(unsigned owner) {
    return atomic_domains[1-owner]?xr_xir_domain_budget_stats(atomic_domains[1-owner]):atomic_final_cost[1-owner];
}
static void atomic_resource_peer_equal(unsigned owner,XrXirDomainBudgetStats before) {
    XrXirDomainBudgetStats after=atomic_resource_peer(owner);
    CHECK(before.requested_bytes==after.requested_bytes && before.requested_call_bytes==after.requested_call_bytes && before.work==after.work);
}
static void atomic_resource_call_failure(AtomicResourceRun *r,unsigned owner,const XrXirCall *call,const XirTaskActivation *activation) {
    if(!call)return;const XrXirTaskExecutor *executor=r->graph.instances[owner]->executor;
    CHECK(activation->call==call);XrXirExecutorBinding binding={executor,(void *)activation,executor->generation,activation->ticket};
    XrXirCallStatus status=XR_XIR_CALL_READY;CHECK(xr_xir_call_driver_failure(call,&binding,&status));
    if(status==r->expected_fault)atomic_resource_fault(r,owner);else CHECK(status==XR_XIR_CALL_READY);
}
static void atomic_resource_accepted(AtomicResourceRun *r,unsigned owner,XrXirInstanceResult event) {
    const XrXirTaskExecutor *executor=r->graph.instances[owner]->executor;
    atomic_resource_call_failure(r,owner,executor->root.call,&executor->root);
    for(const XirTask *task=executor->active;task;task=task->active_next)atomic_resource_call_failure(r,owner,task->call,&task->activation);
    if(executor->first_failure==r->expected_fault)atomic_resource_fault(r,owner);
    else CHECK(executor->first_failure==XR_XIR_CALL_READY);
    if(event.outcome.status==r->expected_fault)atomic_resource_fault(r,owner);
    else CHECK(event.outcome.status==XR_XIR_CALL_READY || event.outcome.status==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_call_result_valid(&event.outcome));
}
static void atomic_resource_pair(AtomicResourceRun *r,unsigned entry,unsigned mask,XrXirValue output[2]) {
    CHECK(entry<ATOMIC_RUNTIME_ENTRIES && mask && mask<=3);r->entry=entry;++r->step;bool done[2]={true,true};unsigned current[2]={0};
    for(unsigned i=0;i<2;++i)if((mask&(1u<<i)) && !r->failed[i] && r->graph.instances[i]) {
        CHECK(r->calls[i]<40);current[i]=r->calls[i]++;r->call_entries[i][current[i]]=entry;
        XrXirDomainBudgetStats peer=atomic_resource_peer(i);
        bool started=atomic_resource_status(r,i,xr_xir_instance_start(r->graph.instances[i],p20_atomic_runtime_entries[entry],NULL,0),XR_XIR_CALL_READY);
        atomic_resource_costs();atomic_resource_peer_equal(i,peer);done[i]=!started;
    }
    unsigned polls=0;
    while(!done[0] || !done[1])for(unsigned i=0;i<2;++i)if(!done[i]) {
        XrXirDomainBudgetStats peer=atomic_resource_peer(i);atomic_observe(&r->graph,i);
        XrXirInstanceResult event=xr_xir_instance_poll_bounded(r->graph.instances[i],1);++r->graph.trace[i].polls;++r->call_polls[i][current[i]];CHECK(++polls<8192);
        atomic_resource_accepted(r,i,event);atomic_resource_costs();atomic_resource_peer_equal(i,peer);
        if(r->failed[i]){done[i]=true;CHECK(!output[i].type && !output[i].payload);continue;}
        atomic_observe(&r->graph,i);
        if(event.outcome.status==XR_XIR_CALL_RETURNED) {
            bool taken=atomic_resource_status(r,i,xr_xir_instance_take_result(r->graph.instances[i],&output[i]),XR_XIR_CALL_RETURNED);
            if(!taken)CHECK(!output[i].type && !output[i].payload);
            else {XrXirValue empty={0};CHECK(xr_xir_instance_take_result(r->graph.instances[i],&empty)==XR_XIR_CALL_BAD_STATE && !empty.type && !empty.payload);}
            atomic_resource_costs();atomic_resource_peer_equal(i,peer);done[i]=true;
        }
    }
}
static void atomic_resource_round(AtomicResourceRun *r) {
    for(unsigned entry=0;entry<10;++entry) {
        XrXirValue result[2]={{0},{0}};atomic_resource_pair(r,entry,3,result);
        for(unsigned i=0;i<2;++i) {
            if(r->failed[i] || !r->graph.instances[i]){CHECK(!result[i].type && !result[i].payload);continue;}
            unsigned phase=r->graph.phase[i];
            if(entry<6) {
                uint64_t n=entry<2?(phase?47u:40u):entry<4?(phase==2?30u:phase?3u:0u):41u;
                atomic_scalar(&result[i],XR_XIR_I64,n);
            }else if(entry<8)atomic_scalar(&result[i],XR_XIR_F64,atomic_f64_bits(entry==6?40.5:phase?2.5:1.5));
            else atomic_scalar(&result[i],XR_XIR_BOOL,entry==8 || phase);
            xr_xir_value_drop(&result[i]);
        }
    }
}
static void atomic_resource_holds(AtomicResourceRun *r) {
    for(unsigned field=0;field<4;++field) {
        XrXirValue values[2]={{0},{0}};atomic_resource_pair(r,10+field,3,values);
        for(unsigned i=0;i<2;++i)if(!r->failed[i] && r->graph.instances[i]) {
            const XirObject *object=(const XirObject *)(uintptr_t)values[i].payload;
            CHECK(xr_xir_value_valid(&values[i]) && object->kind==XR_XIR_TYPE_ATOMIC && object->domain==atomic_domains[i]);
            r->graph.held[i][field]=values[i];
        }
        if(values[0].type && values[1].type)CHECK(values[0].payload!=values[1].payload);
    }
    for(unsigned i=0;i<2;++i)if(!r->failed[i] && r->graph.instances[i]) {
        XrXirValue alias={0};
        if(atomic_resource_value_status(r,i,xr_xir_value_copy(&r->graph.held[i][0],&alias))) {
            CHECK(alias.type==r->graph.held[i][0].type && alias.payload==r->graph.held[i][0].payload);xr_xir_value_drop(&alias);
        }else CHECK(!alias.type && !alias.payload);
    }
}
static bool atomic_resource_operation(AtomicResourceRun *r,unsigned owner,const XrXirValue *value,XrXirAtomicOperation operation,XrXirValue data,uint64_t expected) {
    if(r->failed[owner] || !value->type)return false;
    ++r->step;r->entry=ATOMIC_RUNTIME_ENTRIES;CHECK(xr_xir_value_valid(value));
    const XirObject *object=(const XirObject *)(uintptr_t)value->payload;CHECK(object->domain==atomic_domains[owner]);
    XrXirDomainBudgetStats peer=atomic_resource_peer(owner);XrXirValue out={0};bool write=operation!=XR_XIR_ATOMIC_OPERATION_LOAD;
    XrXirValueAdmission admission={object->arena,object->domain,NULL,NULL,10000,65536};
    XrXirAtomicRequest request={value,write?&data:NULL,NULL,write?1u:0u,operation,(XrXirType)data.type};XrXirAtomicProgress progress={0};
    XrXirAtomicOutcome status=xr_xir_atomic_start(&request,&admission,NULL,&progress,&out);
    xr_xir_atomic_progress_clear(&progress);atomic_resource_costs();atomic_resource_peer_equal(owner,peer);
    CHECK(status.permission==XR_XIR_CALL_READY && !status.continuing);
    if(status.status!=XR_XIR_RUN_OK) {
        CHECK(status.status==(r->expected_fault==XR_XIR_CALL_LIMIT?XR_XIR_RUN_STEP_LIMIT:XR_XIR_RUN_OUT_OF_MEMORY));
        CHECK(!out.type && !out.payload);atomic_resource_fault(r,owner);return false;
    }
    atomic_scalar(&out,(XrXirType)data.type,expected);xr_xir_value_drop(&out);return true;
}
static XrXirValue atomic_resource_operand(XrXirType type,uint64_t payload) {return (XrXirValue){(uint32_t)type,0,(int64_t)payload};}
static void atomic_resource_change(AtomicResourceRun *r) {
    if(!r->failed[0] && r->graph.instances[0]) {
        bool changed=atomic_resource_operation(r,0,&r->graph.held[0][0],XR_XIR_ATOMIC_OPERATION_FETCH_ADD,atomic_resource_operand(XR_XIR_I64,7),40);
        changed=atomic_resource_operation(r,0,&r->graph.held[0][1],XR_XIR_ATOMIC_OPERATION_FETCH_ADD,atomic_resource_operand(XR_XIR_I64,3),0) && changed;
        changed=atomic_resource_operation(r,0,&r->graph.held[0][2],XR_XIR_ATOMIC_OPERATION_SWAP,atomic_resource_operand(XR_XIR_F64,atomic_f64_bits(2.5)),atomic_f64_bits(1.5)) && changed;
        changed=atomic_resource_operation(r,0,&r->graph.held[0][3],XR_XIR_ATOMIC_OPERATION_SWAP,atomic_resource_operand(XR_XIR_BOOL,1),0) && changed;
        if(changed)r->graph.phase[0]=1;
    }
    atomic_resource_round(r);
    XrXirValue unit[2]={{0},{0}};atomic_resource_pair(r,14,1,unit);
    if(!r->failed[0] && r->graph.instances[0]){atomic_scalar(&unit[0],XR_XIR_UNIT,0);xr_xir_value_drop(&unit[0]);r->graph.phase[0]=2;}
    XrXirValue current[2]={{0},{0}};atomic_resource_pair(r,11,3,current);
    if(current[0].type){CHECK(current[0].payload!=r->graph.held[0][1].payload);r->graph.held[0][4]=current[0];}
    xr_xir_value_drop(&current[1]);atomic_resource_round(r);
    (void)atomic_resource_operation(r,0,&r->graph.held[0][1],XR_XIR_ATOMIC_OPERATION_LOAD,atomic_resource_operand(XR_XIR_I64,0),3);
    (void)atomic_resource_operation(r,0,&r->graph.held[0][4],XR_XIR_ATOMIC_OPERATION_LOAD,atomic_resource_operand(XR_XIR_I64,0),30);
    for(unsigned i=0;i<2;++i)r->finished[i]=!r->failed[i] && r->graph.instances[i];
}
static void atomic_resource_retire(AtomicResourceRun *r,unsigned close,unsigned drop) {
    for(unsigned n=0;n<2;++n) {
        unsigned i=n?1-close:close;if(!r->graph.instances[i])continue;
        ++r->step;r->entry=ATOMIC_RUNTIME_ENTRIES+1;XrXirDomainBudgetStats peer=atomic_resource_peer(i);
        XrXirCallStatus status=xr_xir_instance_free(r->graph.instances[i]);r->graph.instances[i]=NULL;
        (void)atomic_resource_status(r,i,status,XR_XIR_CALL_READY);atomic_resource_costs();atomic_resource_peer_equal(i,peer);
    }
    CHECK(r->graph.fixture.releases==1 && !r->graph.fixture.vm && !r->graph.fixture.entries);
    for(unsigned i=0;i<2;++i)if(!r->failed[i] && (r->published_mask&(1u<<i))) {
        (void)atomic_resource_operation(r,i,&r->graph.held[i][0],XR_XIR_ATOMIC_OPERATION_LOAD,atomic_resource_operand(XR_XIR_I64,0),i?40:47);
        (void)atomic_resource_operation(r,i,&r->graph.held[i][1],XR_XIR_ATOMIC_OPERATION_LOAD,atomic_resource_operand(XR_XIR_I64,0),i?0:3);
        (void)atomic_resource_operation(r,i,&r->graph.held[i][2],XR_XIR_ATOMIC_OPERATION_LOAD,atomic_resource_operand(XR_XIR_F64,0),atomic_f64_bits(i?1.5:2.5));
        (void)atomic_resource_operation(r,i,&r->graph.held[i][3],XR_XIR_ATOMIC_OPERATION_SWAP,atomic_resource_operand(XR_XIR_BOOL,i?1:0),i?0:1);
    }
    size_t attempts=runtime_attempts;
    for(unsigned n=0;n<2;++n) {
        unsigned i=n?1-drop:drop;
        for(unsigned field=0;field<5;++field)if(r->graph.held[i][field].type) {
            uintptr_t address=(uintptr_t)r->graph.held[i][field].payload;CHECK(atomic_runtime_physical(address));
            xr_xir_value_drop(&r->graph.held[i][field]);CHECK(!atomic_runtime_physical(address));
        }
        CHECK(!atomic_domains[i] && atomic_domain_disposals[i]==(unsigned)((r->published_mask&(1u<<i))!=0));
    }
    CHECK(runtime_attempts==attempts && !runtime_live && !runtime_bytes);effects_source_owners_free();
}
static AtomicResourceResult atomic_resource_transaction(unsigned backend,unsigned close,unsigned drop,const XrXirInstanceConfig *caps,unsigned limited_owner,size_t failure) {
    CHECK(backend<4 && close<2 && drop<2 && limited_owner<2 && !effects_compile_live && !runtime_live && !atomic_domains[0] && !atomic_domains[1]);
    memset(atomic_final_cost,0,sizeof(atomic_final_cost));memset(atomic_previous_cost,0,sizeof(atomic_previous_cost));memset(atomic_domain_disposals,0,sizeof(atomic_domain_disposals));
    runtime_attempts=0;runtime_fail_at=SIZE_MAX;effects_compile_attempts=0;AtomicResourceRun r={.expected_fault=caps?XR_XIR_CALL_LIMIT:XR_XIR_CALL_OOM,.fault_owner=2};
    XrXirProgram *program=NULL;CHECK(atomic_runtime_prepare(&r.graph.fixture,backend,false,&program) && program && !runtime_live);
    for(unsigned i=0;i<2;++i)CHECK(xr_xir_instance_config_init(&atomic_resource_configs[i],sizeof(atomic_resource_configs[i]))==XR_XIR_CALL_READY);
    if(caps)atomic_resource_configs[limited_owner]=*caps;
    runtime_attempts=0;runtime_fail_at=failure;
    for(unsigned i=0;i<2;++i) {
        ++r.step;r.entry=ATOMIC_RUNTIME_ENTRIES+2;
        if(!atomic_resource_status(&r,i,xr_xir_instance_new(program,&atomic_resource_configs[i],&r.graph.instances[i]),XR_XIR_CALL_READY)){CHECK(!r.graph.instances[i]);continue;}
        r.published_mask|=1u<<i;atomic_domains[i]=r.graph.instances[i]->domain;atomic_resource_costs();
    }
    if(r.published_mask==3)CHECK(atomic_domains[0]!=atomic_domains[1]);xr_xir_compile_program_drop(program);
    atomic_resource_round(&r);atomic_resource_holds(&r);atomic_resource_change(&r);
    atomic_resource_retire(&r,close,drop);AtomicResourceResult out={0};
    for(unsigned i=0;i<2;++i) {
        out.cost[i]=atomic_final_cost[i];out.fault_cost[i]=r.fault_cost[i];out.unaffected[i]=r.finished[i] && !r.failed[i];
        out.polls[i]=r.graph.trace[i].polls;out.tasks[i]=r.graph.trace[i].tasks;out.root_mask[i]=r.graph.trace[i].root_mask;out.worker_mask[i]=r.graph.trace[i].worker_mask;out.disposals+=atomic_domain_disposals[i];
        if(r.published_mask&(1u<<i))CHECK(out.cost[i].requested_bytes<=atomic_resource_configs[i].requested_value_limit && out.cost[i].requested_call_bytes<=atomic_resource_configs[i].requested_call_limit && out.cost[i].work<=atomic_resource_configs[i].work_limit);
        else CHECK(!out.cost[i].requested_bytes && !out.cost[i].requested_call_bytes && !out.cost[i].work);
    }
    memcpy(out.calls,r.calls,sizeof(out.calls));memcpy(out.call_entries,r.call_entries,sizeof(out.call_entries));memcpy(out.call_polls,r.call_polls,sizeof(out.call_polls));
    out.sites=runtime_attempts;out.hit=failure!=SIZE_MAX && runtime_attempts>failure;out.complete=!r.accepted_fault && out.unaffected[0] && out.unaffected[1];
    out.fault=r.accepted_fault;out.fault_owner=r.fault_owner;out.fault_step=r.fault_step;out.fault_entry=r.fault_entry;out.fault_observed=r.fault_observed;out.published_mask=r.published_mask;
    runtime_fail_at=SIZE_MAX;CHECK(!effects_compile_live && !effects_compile_bytes && !runtime_live && !runtime_bytes);return out;
}
static AtomicResourceResult atomic_resource_baseline(unsigned backend,unsigned close,unsigned drop) {
    AtomicResourceResult r=atomic_resource_transaction(backend,close,drop,NULL,0,SIZE_MAX);
    CHECK(r.complete && r.published_mask==3 && r.disposals==2 && r.sites==437 && !r.fault);
    for(unsigned i=0;i<2;++i)CHECK(r.tasks[i]==30 && r.polls[i]==(i?543u:555u) && r.root_mask[i]==(atomic_runtime_native(backend,true)?2u:1u) && r.worker_mask[i]==(atomic_runtime_native(backend,false)?2u:1u));
    printf("Atomic resources backend=%u close=%u drop=%u sites=%zu polls=%u/%u tasks=%u/%u value0=%llu call0=%llu work0=%llu value1=%llu call1=%llu work1=%llu originalDomains2 lastReferenceZero2 codeLease1 lateAtomicAfterCodeDeath physical0/0\n",
        backend,close,drop,r.sites,r.polls[0],r.polls[1],r.tasks[0],r.tasks[1],(unsigned long long)r.cost[0].requested_bytes,(unsigned long long)r.cost[0].requested_call_bytes,(unsigned long long)r.cost[0].work,(unsigned long long)r.cost[1].requested_bytes,(unsigned long long)r.cost[1].requested_call_bytes,(unsigned long long)r.cost[1].work);
    for(unsigned i=0;i<2;++i) {
        CHECK(r.calls[i]==(i?35u:36u));unsigned polls=0;
        for(unsigned call=0;call<r.calls[i];++call) {
            polls+=r.call_polls[i][call];
            printf("Atomic resource publicCall backend=%u close=%u drop=%u owner=%u ordinal=%u entry=%u polls=%u\n",backend,close,drop,i,call,r.call_entries[i][call],r.call_polls[i][call]);
        }
        CHECK(polls==r.polls[i]);
    }
    fflush(stdout);return r;
}
static void atomic_resource_axes(unsigned backend,unsigned close,unsigned drop,const AtomicResourceResult *normal) {
    for(unsigned owner=0;owner<2;++owner) {
        uint64_t fees[]={normal->cost[owner].requested_bytes,normal->cost[owner].requested_call_bytes,normal->cost[owner].work};
        for(unsigned axis=0;axis<3;++axis)for(unsigned minus=0;minus<2;++minus) {
            CHECK(fees[axis]>1);XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
            uint64_t cap=fees[axis]-minus;if(!axis)config.requested_value_limit=cap;else if(axis==1)config.requested_call_limit=cap;else config.work_limit=cap;
            AtomicResourceResult r=atomic_resource_transaction(backend,close,drop,&config,owner,SIZE_MAX);
            printf("Atomic resource axis backend=%u close=%u drop=%u owner=%u axis=%u minus=%u cap=%llu complete=%u fault=%u faultOwner=%u faultStep=%u faultEntry=%u faultObserved=%u publishedMask=%u unaffected=%u/%u value0=%llu call0=%llu work0=%llu value1=%llu call1=%llu work1=%llu physical0/0\n",
                backend,close,drop,owner,axis,minus,(unsigned long long)cap,(unsigned)r.complete,r.fault,r.fault_owner,r.fault_step,r.fault_entry,r.fault_observed,r.published_mask,(unsigned)r.unaffected[0],(unsigned)r.unaffected[1],(unsigned long long)r.cost[0].requested_bytes,(unsigned long long)r.cost[0].requested_call_bytes,(unsigned long long)r.cost[0].work,(unsigned long long)r.cost[1].requested_bytes,(unsigned long long)r.cost[1].requested_call_bytes,(unsigned long long)r.cost[1].work);fflush(stdout);
            CHECK(r.complete==!minus && r.fault==(unsigned)(minus?XR_XIR_CALL_LIMIT:XR_XIR_CALL_READY) && r.unaffected[1-owner]);
            if(minus)CHECK(r.fault_owner==owner);
            for(unsigned i=0;i<2;++i)if(!minus || i!=owner)CHECK(r.cost[i].requested_bytes==normal->cost[i].requested_bytes && r.cost[i].requested_call_bytes==normal->cost[i].requested_call_bytes && r.cost[i].work==normal->cost[i].work);
        }
    }
}
#endif // XIR_MIXED_ATOMIC_RESOURCES_H
