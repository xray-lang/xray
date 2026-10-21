/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_mixed_atomic_lifetime.c - Source worker permissions and late shared cells
 */
#include "xir_mixed_atomic_fixture.h"
typedef struct AtomicTrace {
    uint64_t task_ids[64];
    unsigned root_mask,worker_mask,tasks,task_observations,polls;
} AtomicTrace;
typedef struct AtomicGraph {
    AtomicRuntimeFixture fixture;
    XrXirInstance *instances[2];
    XrXirValue held[2][5];
    AtomicTrace trace[2];
    unsigned phase[2];
} AtomicGraph;
static bool atomic_runtime_physical(uintptr_t address) {
    for(size_t i=0;i<runtime_owned_capacity;++i)if(runtime_owned[i].pointer) {
        uintptr_t base=(uintptr_t)runtime_owned[i].pointer;
        if(address>=base && address-base<runtime_owned[i].bytes)return true;
    }
    return false;
}
static uint64_t atomic_f64_bits(double value) {uint64_t bits=0;memcpy(&bits,&value,sizeof(bits));return bits;}
static void atomic_frames(AtomicGraph *g,unsigned owner,XrXirCall *call,bool child) {
    CHECK(call && call->config.admission.domain==atomic_domains[owner]);unsigned depth=0;
    for(const CallFrame *frame=call->top;frame;frame=frame->parent) {
        CHECK(++depth<64);uintptr_t base=(uintptr_t)call->config.entries,at=(uintptr_t)frame->entry;
        CHECK(at>=base && !((at-base)%sizeof(*frame->entry)));size_t f=(at-base)/sizeof(*frame->entry);
        CHECK(f<call->config.entry_count);if(f==g->fixture.count || !frame->entered)continue;
        CHECK(f<g->fixture.count);bool native=atomic_runtime_native(g->fixture.backend,g->fixture.root[f]);
        const XrXirCallEntry *expected=native?&p20_atomic_runtime_fixture_program.entries[f]:&g->fixture.vm->entries[f];
        CHECK(frame->entry->resume==expected->resume && frame->entry->environment==expected->environment);
        CHECK(frame->code_kind==(native?XR_XIR_CALL_CODE_NATIVE:XR_XIR_CALL_CODE_VM));
        if(child){CHECK(!g->fixture.root[f]);g->trace[owner].worker_mask|=native?2u:1u;}
        else if(g->fixture.root[f])g->trace[owner].root_mask|=native?2u:1u;
    }
}
static void atomic_observe(AtomicGraph *g,unsigned owner) {
    XrXirInstance *instance=g->instances[owner];CHECK(instance && !instance->driving && instance->domain==atomic_domains[owner]);
    atomic_frames(g,owner,instance->call,false);unsigned count=0;
    for(const XirTask *task=instance->executor->active;task;task=task->active_next) {
        CHECK(++count<8 && task->executor==instance->executor && task->object.domain==instance->domain);
        CHECK(task->call && task->activation.call==task->call && task->activation.task==task);
        CHECK(task->call->executor_owner==instance->executor && task->call->executor_activation==&task->activation);
        atomic_frames(g,owner,task->call,true);++g->trace[owner].task_observations;
        unsigned at=0;while(at<g->trace[owner].tasks && g->trace[owner].task_ids[at]!=task->identity)++at;
        if(at==g->trace[owner].tasks){CHECK(at<64);g->trace[owner].task_ids[at]=task->identity;++g->trace[owner].tasks;}
    }
}
static void atomic_pair(AtomicGraph *g,unsigned entry,XrXirValue output[2]) {
    CHECK(entry<ATOMIC_RUNTIME_ENTRIES);bool done[2]={0};unsigned polls=0;
    for(unsigned i=0;i<2;++i)CHECK(xr_xir_instance_start(g->instances[i],p20_atomic_runtime_entries[entry],NULL,0)==XR_XIR_CALL_READY);
    while(!done[0] || !done[1])for(unsigned i=0;i<2;++i)if(!done[i]) {
        unsigned peer=1-i;XrXirDomainBudgetStats before=xr_xir_domain_budget_stats(atomic_domains[peer]);
        atomic_observe(g,i);XrXirInstanceResult event=xr_xir_instance_poll_bounded(g->instances[i],1);
        atomic_observe(g,i);atomic_runtime_cost(i);++g->trace[i].polls;CHECK(++polls<4096);
        XrXirDomainBudgetStats after=xr_xir_domain_budget_stats(atomic_domains[peer]);
        CHECK(before.requested_bytes==after.requested_bytes && before.requested_call_bytes==after.requested_call_bytes && before.work==after.work);
        CHECK(event.outcome.status==XR_XIR_CALL_READY || event.outcome.status==XR_XIR_CALL_RETURNED);
        if(event.outcome.status==XR_XIR_CALL_RETURNED) {
            CHECK(xr_xir_call_result_valid(&event.outcome));
            CHECK(xr_xir_instance_take_result(g->instances[i],&output[i])==XR_XIR_CALL_RETURNED);
            XrXirValue empty={0};CHECK(xr_xir_instance_take_result(g->instances[i],&empty)==XR_XIR_CALL_BAD_STATE && !empty.type && !empty.payload);done[i]=true;
        }
    }
}
static void atomic_scalar(const XrXirValue *value,XrXirType type,uint64_t payload) {
    CHECK(value->type==(uint32_t)type && !value->reserved && (uint64_t)value->payload==payload && xr_xir_value_valid(value));
}
static void atomic_round(AtomicGraph *g) {
    for(unsigned entry=0;entry<10;++entry) {
        XrXirValue result[2]={{0},{0}};atomic_pair(g,entry,result);
        for(unsigned i=0;i<2;++i) {
            unsigned phase=g->phase[i];
            if(entry<6) {
                uint64_t n=entry<2?(phase?47u:40u):entry<4?(phase==2?30u:phase?3u:0u):41u;
                atomic_scalar(&result[i],XR_XIR_I64,n);
            }else if(entry<8)atomic_scalar(&result[i],XR_XIR_F64,atomic_f64_bits(entry==6?40.5:phase?2.5:1.5));
            else atomic_scalar(&result[i],XR_XIR_BOOL,entry==8 || phase);
            xr_xir_value_drop(&result[i]);
        }
    }
}
static XrXirValue atomic_operation(const XrXirValue *value,XrXirAtomicOperation operation,XrXirType type,uint64_t operand) {
    CHECK(xr_xir_value_valid(value));const XirObject *object=(const XirObject *)(uintptr_t)value->payload;
    XrXirValue data={type,0,operand},out={0};bool write=operation!=XR_XIR_ATOMIC_OPERATION_LOAD;
    XrXirValueAdmission admission={object->arena,object->domain,NULL,NULL,10000,65536};
    XrXirAtomicRequest request={value,write?&data:NULL,NULL,write?1u:0u,operation,type};XrXirAtomicProgress progress={0};
    XrXirAtomicOutcome status=xr_xir_atomic_start(&request,&admission,NULL,&progress,&out);
    CHECK(status.status==XR_XIR_RUN_OK && status.permission==XR_XIR_CALL_READY && !status.continuing);
    xr_xir_atomic_progress_clear(&progress);CHECK(xr_xir_value_valid(&out));return out;
}
static void atomic_check_operation(const XrXirValue *value,XrXirAtomicOperation operation,XrXirType type,uint64_t operand,uint64_t expected) {
    XrXirValue out=atomic_operation(value,operation,type,operand);atomic_scalar(&out,type,expected);xr_xir_value_drop(&out);
}
static void atomic_holds(AtomicGraph *g) {
    for(unsigned field=0;field<4;++field) {
        XrXirValue values[2]={{0},{0}};atomic_pair(g,10+field,values);
        for(unsigned i=0;i<2;++i) {
            const XirObject *object=(const XirObject *)(uintptr_t)values[i].payload;
            CHECK(xr_xir_value_valid(&values[i]) && object->kind==XR_XIR_TYPE_ATOMIC && object->domain==atomic_domains[i]);
            CHECK(values[0].payload!=values[1].payload);g->held[i][field]=values[i];
        }
    }
    for(unsigned i=0;i<2;++i) {
        XrXirValue alias={0};CHECK(xr_xir_value_copy(&g->held[i][0],&alias)==XR_XIR_VALUE_OK);
        CHECK(alias.type==g->held[i][0].type && alias.payload==g->held[i][0].payload);xr_xir_value_drop(&alias);
    }
}
static void atomic_change(AtomicGraph *g) {
    XrXirDomainBudgetStats peer=xr_xir_domain_budget_stats(atomic_domains[1]);
    atomic_check_operation(&g->held[0][0],XR_XIR_ATOMIC_OPERATION_FETCH_ADD,XR_XIR_I64,7,40);
    atomic_check_operation(&g->held[0][1],XR_XIR_ATOMIC_OPERATION_FETCH_ADD,XR_XIR_I64,3,0);
    atomic_check_operation(&g->held[0][2],XR_XIR_ATOMIC_OPERATION_SWAP,XR_XIR_F64,atomic_f64_bits(2.5),atomic_f64_bits(1.5));
    atomic_check_operation(&g->held[0][3],XR_XIR_ATOMIC_OPERATION_SWAP,XR_XIR_BOOL,1,0);g->phase[0]=1;
    XrXirDomainBudgetStats after=xr_xir_domain_budget_stats(atomic_domains[1]);
    CHECK(peer.requested_bytes==after.requested_bytes && peer.requested_call_bytes==after.requested_call_bytes && peer.work==after.work);
    atomic_round(g);
    peer=xr_xir_domain_budget_stats(atomic_domains[1]);
    CHECK(xr_xir_instance_start(g->instances[0],p20_atomic_runtime_entries[14],NULL,0)==XR_XIR_CALL_READY);
    unsigned polls=0;XrXirInstanceResult event={0};
    do{atomic_observe(g,0);event=xr_xir_instance_poll_bounded(g->instances[0],1);atomic_observe(g,0);atomic_runtime_cost(0);++g->trace[0].polls;CHECK(++polls<4096);}while(event.outcome.status==XR_XIR_CALL_READY);
    CHECK(event.outcome.status==XR_XIR_CALL_RETURNED);XrXirValue unit={0};CHECK(xr_xir_instance_take_result(g->instances[0],&unit)==XR_XIR_CALL_RETURNED);
    atomic_scalar(&unit,XR_XIR_UNIT,0);xr_xir_value_drop(&unit);g->phase[0]=2;
    after=xr_xir_domain_budget_stats(atomic_domains[1]);
    CHECK(peer.requested_bytes==after.requested_bytes && peer.requested_call_bytes==after.requested_call_bytes && peer.work==after.work);
    XrXirValue current[2]={{0},{0}};atomic_pair(g,11,current);g->held[0][4]=current[0];xr_xir_value_drop(&current[1]);
    CHECK(g->held[0][4].payload!=g->held[0][1].payload);atomic_round(g);
    atomic_check_operation(&g->held[0][1],XR_XIR_ATOMIC_OPERATION_LOAD,XR_XIR_I64,0,3);
    atomic_check_operation(&g->held[0][4],XR_XIR_ATOMIC_OPERATION_LOAD,XR_XIR_I64,0,30);
}
static void atomic_retire(AtomicGraph *g,unsigned close,unsigned drop) {
    for(unsigned n=0;n<2;++n) {
        unsigned i=n?1-close:close;CHECK(xr_xir_instance_free(g->instances[i])==XR_XIR_CALL_READY);g->instances[i]=NULL;
        CHECK(g->fixture.releases==(unsigned)(n==1));
    }
    CHECK(!g->fixture.vm && !g->fixture.entries && g->fixture.releases==1);size_t compiler_live=effects_compile_live;
    for(unsigned i=0;i<2;++i) {
        atomic_check_operation(&g->held[i][0],XR_XIR_ATOMIC_OPERATION_LOAD,XR_XIR_I64,0,i?40:47);
        atomic_check_operation(&g->held[i][1],XR_XIR_ATOMIC_OPERATION_LOAD,XR_XIR_I64,0,i?0:3);
        atomic_check_operation(&g->held[i][2],XR_XIR_ATOMIC_OPERATION_LOAD,XR_XIR_F64,0,atomic_f64_bits(i?1.5:2.5));
        atomic_check_operation(&g->held[i][3],XR_XIR_ATOMIC_OPERATION_SWAP,XR_XIR_BOOL,i?1:0,i?0:1);
    }
    size_t attempts=runtime_attempts;
    for(unsigned n=0;n<2;++n) {
        unsigned i=n?1-drop:drop;
        for(unsigned field=0;field<5;++field)if(g->held[i][field].type) {
            uintptr_t address=(uintptr_t)g->held[i][field].payload;CHECK(atomic_runtime_physical(address));
            xr_xir_value_drop(&g->held[i][field]);CHECK(!atomic_runtime_physical(address));
        }
        CHECK(!atomic_domains[i] && atomic_domain_disposals[i]==1);
    }
    CHECK(runtime_attempts==attempts && !runtime_live && !runtime_bytes);effects_source_owners_free();
    printf("Atomic runtime backend=%u close=%u drop=%u rootMask=%u/%u workerMask=%u/%u tasks=%u/%u taskObservations=%u/%u polls=%u/%u sites=%zu compilerLiveAfterCodeDeath=%zu originalDomains2 lastReferenceZero2 sharedAlias i64_40to47 f64_1p5to2p5 boolSwap rootSnapshot rebindOld3New30 peerUnchanged codeLease1 lateAtomicAfterCodeDeath physical0/0\n",
        g->fixture.backend,close,drop,g->trace[0].root_mask,g->trace[1].root_mask,g->trace[0].worker_mask,g->trace[1].worker_mask,g->trace[0].tasks,g->trace[1].tasks,g->trace[0].task_observations,g->trace[1].task_observations,g->trace[0].polls,g->trace[1].polls,runtime_attempts,compiler_live);
}
static void atomic_graph(unsigned backend,unsigned close,unsigned drop) {
    CHECK(!effects_compile_live && !runtime_live && !atomic_domains[0] && !atomic_domains[1]);
    memset(atomic_final_cost,0,sizeof(atomic_final_cost));memset(atomic_previous_cost,0,sizeof(atomic_previous_cost));memset(atomic_domain_disposals,0,sizeof(atomic_domain_disposals));
    runtime_attempts=0;effects_compile_attempts=0;AtomicGraph g={0};XrXirProgram *program=NULL;
    CHECK(atomic_runtime_prepare(&g.fixture,backend,false,&program) && program && !runtime_live);
    for(unsigned i=0;i<2;++i) {
        XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init_sandbox(&config,sizeof(config))==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_new(program,&config,&g.instances[i])==XR_XIR_CALL_READY);atomic_domains[i]=g.instances[i]->domain;
    }
    CHECK(atomic_domains[0]!=atomic_domains[1]);xr_xir_compile_program_drop(program);
    atomic_round(&g);atomic_holds(&g);atomic_change(&g);
    for(unsigned i=0;i<2;++i)CHECK(g.trace[i].root_mask==(atomic_runtime_native(backend,true)?2u:1u) && g.trace[i].worker_mask==(atomic_runtime_native(backend,false)?2u:1u) && g.trace[i].tasks==30);
    CHECK(runtime_attempts && runtime_attempts<20000);atomic_retire(&g,close,drop);
}
int main(int argc,char **argv) {
    CHECK(argc==2);const char *names[]={"0","1","vm","native"};unsigned backend=0;
    while(backend<4 && strcmp(argv[1],names[backend]))++backend;CHECK(backend<4);
    for(unsigned close=0;close<2;++close)for(unsigned drop=0;drop<2;++drop)atomic_graph(backend,close,drop);
    return 0;
}
