/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_mixed_cycle_batch_resources.h - Whole original owners across Source closure retirement
 */
#ifndef XIR_MIXED_CYCLE_BATCH_RESOURCES_H
#define XIR_MIXED_CYCLE_BATCH_RESOURCES_H
#include "xir/xxir_vm.h"
#include "xir/xxir_checked.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if(!(c)){fprintf(stderr,"Cycle resources %d: %s\n",__LINE__,#c);exit(1);} } while(0)
#include "xir_effect_execution_owner.h"
static void cr_before_free(void *pointer);
#define XIR_RUNTIME_BEFORE_FREE(pointer) cr_before_free(pointer)
#include "xir_runtime_allocations.h"
#undef XIR_RUNTIME_BEFORE_FREE
#undef CHECK
#define main cycle_resource_original_main
#define p20_cell_cycle_fixture_program p20_cycle_batch_fixture_program
#define p20_cell_cycle_entries p20_cycle_batch_pair_entries
#include "test_xir_mixed_cell_cycles.c"
#undef p20_cell_cycle_entries
#undef p20_cell_cycle_fixture_program
#undef main
#undef CHECK
#define CHECK(c) do { if(!(c)){fprintf(stderr,"Cycle resources %d: %s\n",__LINE__,#c);exit(1);} } while(0)
XR_DATA const uint32_t p20_cycle_batch_entries[3];
typedef struct CycleResourceSpec { unsigned backend,shape,count,kind,mode,prefix,close,drop; } CycleResourceSpec;
typedef struct CycleResourceResult {
    XrXirDomainBudgetStats cost[2];
    size_t sites,setup;
    unsigned polls,phase,published,disposals,observed,frees,mask,terminal,status,before_edges[2];
    bool complete,hit;
} CycleResourceResult;
typedef struct CycleResourceRun {
    CellCycleFixture fixture;
    XrXirInstance *instances[2];
    XrXirValue functions[17],aliases[17];
    XrXirValue committed[2];
    const XrXirFunctionBinding *observed_binding;
    uintptr_t target;
    XrXirCallStatus fault;
    unsigned captures[17],main_count,count,phase,polls,yields,mask,terminal,status,before_edges[2];
    bool complete,published[2],policy,selected;
} CycleResourceRun;
typedef struct CycleObservedObject { uintptr_t pointer; unsigned owner,kind; bool live; } CycleObservedObject;
static bool cr_diagnostic;
static XrXirDomain *cr_borrowed[2];
static XrXirDomainBudgetStats cr_final[2],cr_previous[2];
static unsigned cr_disposals[2],cr_object_count,cr_object_frees;
static CycleObservedObject cr_objects[2048];
static void cr_monotone(unsigned owner,XrXirDomainBudgetStats cost) {
    CHECK(owner<2 && cost.requested_bytes>=cr_previous[owner].requested_bytes && cost.requested_call_bytes>=cr_previous[owner].requested_call_bytes && cost.work>=cr_previous[owner].work);cr_previous[owner]=cost;
}
/* The observer borrows the actual owner and releases no extra Domain reference. */
static void cr_before_free(void *pointer) {
    if(!pointer)return;
    for(unsigned i=0;i<cr_object_count;++i)if(cr_objects[i].live && cr_objects[i].pointer==(uintptr_t)pointer){cr_objects[i].live=false;++cr_object_frees;}
    for(unsigned i=0;i<2;++i)if(pointer==cr_borrowed[i]) {
        XrXirDomain *d=pointer;CHECK(!atomic_load_explicit(&d->references,memory_order_relaxed) && !d->objects);
        CHECK(d->stats.live_bytes==sizeof(*d) && d->stats.allocations==d->stats.frees+1);
        CHECK(!d->budget.call_live && !d->budget.metadata_live && d->budget.call_allocations==d->budget.call_frees && d->budget.metadata_allocations==d->budget.metadata_frees);
        cr_monotone(i,d->budget);cr_final[i]=d->budget;CHECK(!cr_disposals[i]++);cr_borrowed[i]=NULL;
    }
}
static void cr_cost(void) {for(unsigned i=0;i<2;++i)if(cr_borrowed[i])cr_monotone(i,xr_xir_domain_budget_stats(cr_borrowed[i]));}
static void cr_objects_observe(void) {
    for(unsigned owner=0;owner<2;++owner)if(cr_borrowed[owner]) {
        unsigned visited=0;for(const XirObject *o=cr_borrowed[owner]->objects;o;o=o->graph_next) {
            CHECK(++visited<2048 && o->domain==cr_borrowed[owner] && o->graph_registered);
            bool known=false;for(unsigned j=0;j<cr_object_count;++j)if(cr_objects[j].live && cr_objects[j].pointer==(uintptr_t)o){CHECK(cr_objects[j].owner==owner && cr_objects[j].kind==o->kind);known=true;}
            if(!known){CHECK(cr_object_count<2048);cr_objects[cr_object_count++]=(CycleObservedObject){(uintptr_t)o,owner,o->kind,true};}
        }
    }cr_cost();
}
static bool cr_status(CycleResourceRun *run,XrXirCallStatus status,XrXirCallStatus expected) {
    if(status==run->fault){if(cr_diagnostic)fprintf(stderr,"Cycle diagnose status=%u expected=%u phase=%u work0=%llu limit0=%llu work1=%llu limit1=%llu\n",status,expected,run->phase,(unsigned long long)cr_previous[0].work,(unsigned long long)cr_previous[0].work_limit,(unsigned long long)cr_previous[1].work,(unsigned long long)cr_previous[1].work_limit);run->complete=false;return false;}CHECK(status==expected);return true;
}
static bool cr_value_status(CycleResourceRun *run,XrXirValueStatus status) {
    if(status==XR_XIR_VALUE_OK)return true;CHECK(status==(run->fault==XR_XIR_CALL_LIMIT?XR_XIR_VALUE_LIMIT:XR_XIR_VALUE_OOM));run->complete=false;return false;
}
static const XrXirValue *cr_cell(const XrXirValue *function,unsigned c) {
    CHECK(xr_xir_value_valid(function));const XrXirFunctionBinding *b=xr_xir_function_binding(function);CHECK(b && b->capture_count && b->capture_count<=2 && c<b->capture_count && b->captures && b->release==function_gate_drop);return &b->captures[c];
}
static XrXirInstanceResult cr_tick(CycleResourceRun *run,unsigned owner) {
    unsigned peer=1-owner;XrXirDomainBudgetStats before=xr_xir_domain_budget_stats(cr_borrowed[peer]);
    XrXirInstanceResult event=xr_xir_instance_poll_bounded(run->instances[owner],1);cr_objects_observe();
    CellCycleWitness witness={0};cycle_observe(&run->fixture,run->instances[owner],&witness,0,UINT32_MAX);
    XrXirDomainBudgetStats after=xr_xir_domain_budget_stats(cr_borrowed[peer]);CHECK(before.requested_bytes==after.requested_bytes && before.requested_call_bytes==after.requested_call_bytes && before.work==after.work);
    if(cr_diagnostic)fprintf(stderr,"Cycle diagnose poll owner=%u status=%u phase=%u work=%llu limit=%llu localWork=%llu exhausted=%u callStatus=%u\n",owner,event.outcome.status,run->phase,(unsigned long long)xr_xir_domain_budget_stats(cr_borrowed[owner]).work,(unsigned long long)xr_xir_domain_budget_stats(cr_borrowed[owner]).work_limit,(unsigned long long)run->instances[owner]->call->config.admission.work,(unsigned)run->instances[owner]->budget.exhausted,xr_xir_call_state(run->instances[owner]->call));
    if(event.outcome.status==run->fault)run->complete=false;
    else CHECK(event.outcome.status==XR_XIR_CALL_READY || event.outcome.status==XR_XIR_CALL_RETURNED || event.outcome.status==XR_XIR_CALL_SUSPENDED || event.outcome.status==XR_XIR_CALL_CANCELLED);
    CHECK(xr_xir_call_result_valid(&event.outcome));return event;
}
static void cr_resume(CycleResourceRun *run,unsigned owner,XrXirInstanceResult event) {
    CHECK(event.outcome.status==XR_XIR_CALL_SUSPENDED && event.epoch && event.outcome.wake);XrXirWaitRequest request={0};
    CHECK(xr_xir_instance_wait_request(run->instances[owner],event.epoch,event.outcome.wake,&request)==XR_XIR_CALL_READY && request.kind==XR_XIR_WAIT_YIELD && !request.reserved);
    CHECK(xr_xir_instance_resume(run->instances[owner],event.epoch+1,event.outcome.wake)==XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_instance_resume(run->instances[owner],event.epoch,event.outcome.wake+1)==XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_instance_resume(run->instances[owner],event.epoch,event.outcome.wake)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_resume(run->instances[owner],event.epoch,event.outcome.wake)==XR_XIR_CALL_BAD_STATE);++run->yields;
}
static bool cr_take(CycleResourceRun *run,unsigned owner,unsigned at,unsigned shape) {
    XrXirCallStatus status=xr_xir_instance_take_result(run->instances[owner],&run->functions[at]);cr_objects_observe();
    if(!cr_status(run,status,XR_XIR_CALL_RETURNED)){CHECK(!run->functions[at].type && !run->functions[at].payload);return false;}
    const XrXirFunctionBinding *b=xr_xir_function_binding(&run->functions[at]);CHECK(b && b->capture_count==shape+1 && b->release==function_gate_drop && ((const FunctionGate *)b->owner)->domain==cr_borrowed[owner]);run->captures[at]=b->capture_count;
    for(unsigned c=0;c<b->capture_count;++c)CHECK(xr_xir_cell_in_domain(&b->captures[c],cr_borrowed[owner]));
    if(!cr_value_status(run,xr_xir_value_copy(&run->functions[at],&run->aliases[at]))){CHECK(!run->aliases[at].type && !run->aliases[at].payload);return false;}return true;
}
static bool cr_start_make(CycleResourceRun *run,unsigned owner,unsigned shape,unsigned mode) {
    ++run->phase;XrXirValue args[]={{XR_XIR_I64,0,shape},{XR_XIR_I64,0,mode}};
    XrXirCallStatus status=xr_xir_instance_start(run->instances[owner],p20_cycle_batch_entries[0],args,2);cr_objects_observe();return cr_status(run,status,XR_XIR_CALL_READY);
}
static bool cr_make(CycleResourceRun *run,unsigned owner,unsigned at,unsigned shape) {
    if(!cr_start_make(run,owner,shape,0))return false;XrXirInstanceResult event={0};unsigned polls=0,yields=run->yields;
    do {if(event.outcome.status==XR_XIR_CALL_SUSPENDED)cr_resume(run,owner,event);event=cr_tick(run,owner);CHECK(++polls<4096);if(!run->complete)return false;}while(event.outcome.status!=XR_XIR_CALL_RETURNED);
    CHECK(run->yields==yields+1);return cr_take(run,owner,at,shape);
}
static bool cr_wire(CycleResourceRun *run,unsigned count) {
    if(!count)return true;XrXirValueAdmission admission=instance_candidate_admission(run->instances[0]);CHECK(admission.function==admit_function_binding && admission.context==run->instances[0] && admission.domain==cr_borrowed[0]);
    for(unsigned i=0;i<count;++i)for(unsigned c=0;c<run->captures[i];++c) {
        const XrXirValue *cell=cr_cell(&run->functions[i],c);const XirCell *object=(const XirCell *)object_pointer(cell);XrXirValue before=object->value;
        XrXirValueStatus status=xr_xir_cell_write(cell,&run->functions[(i+1+c)%count],&admission);cr_objects_observe();
        if(!cr_value_status(run,status)){CHECK(!memcmp(&before,&object->value,sizeof(before)));return false;}
        CHECK(object->value.type==run->functions[(i+1+c)%count].type && object->value.payload==run->functions[(i+1+c)%count].payload);
    }return true;
}
static bool cr_reject(CycleResourceRun *run,unsigned left,unsigned right,unsigned owner) {
    for(unsigned c=0;c<run->captures[left];++c)for(unsigned bad=0;bad<2;++bad) {
        const XrXirValue *cell=cr_cell(&run->functions[left],c);const XirCell *object=(const XirCell *)object_pointer(cell);XrXirValue before=object->value;
        XrXirValueAdmission admission=instance_candidate_admission(run->instances[bad?1-owner:owner]);size_t attempts=runtime_attempts;
        XrXirValueStatus status=xr_xir_cell_write(cell,&run->functions[right],&admission);cr_objects_observe();
        CHECK(!memcmp(&before,&object->value,sizeof(before)) && attempts==runtime_attempts);
        if(status!=XR_XIR_VALUE_BAD_ARGUMENT){CHECK(status==XR_XIR_VALUE_LIMIT || status==XR_XIR_VALUE_OOM);return cr_value_status(run,status);}
    }return true;
}
static bool cr_start_break(CycleResourceRun *run) {
    ++run->phase;XrXirValue command={XR_XIR_I64,0,0};XrXirCallStatus status=xr_xir_instance_start_function(run->instances[0],&run->functions[0],&command,1);cr_objects_observe();return cr_status(run,status,XR_XIR_CALL_READY);
}
static const XrXirFunctionBinding *cr_selected_binding(CycleResourceRun *run,const CycleResourceSpec *spec) {
    if(spec->kind==2)return xr_xir_function_binding(&run->functions[0]);
    if(run->functions[spec->count-1].type)return xr_xir_function_binding(&run->functions[spec->count-1]);
    if(!cr_borrowed[0])return NULL;
    for(const XirObject *o=cr_borrowed[0]->objects;o;o=o->graph_next)if(o->kind==XIR_OBJECT_FUNCTION) {
        const XirFunction *f=(const XirFunction *)o;if(f->binding.capture_count!=spec->shape+1 || f->binding.release!=function_gate_drop)continue;
        bool old=false;for(unsigned i=0;i<run->main_count;++i)if((uintptr_t)o==(uintptr_t)run->functions[i].payload)old=true;if(!old)return &f->binding;
    }return NULL;
}
/* Snapshot borrowed descriptors before control; no synthetic owned values are created. */
static void cr_policy(CycleResourceRun *run,const CycleResourceSpec *spec,XrXirInstanceResult event) {
    run->terminal=event.outcome.status==XR_XIR_CALL_RETURNED;const XrXirFunctionBinding *b=cr_selected_binding(run,spec);run->observed_binding=b;run->before_edges[0]=run->before_edges[1]=UINT32_MAX;
    if(b){CHECK(b->capture_count==spec->shape+1);for(unsigned c=0;c<b->capture_count;++c){const XirCell *cell=(const XirCell *)object_pointer(&b->captures[c]);CHECK(cell && cell->object.domain==cr_borrowed[0]);run->committed[c]=cell->value;const XrXirFunctionBinding *edge=xr_xir_function_binding(&cell->value);if(edge){run->before_edges[c]=edge->entry;if(!edge->capture_count)run->mask|=1u<<c;}}}
    run->policy=true;
}
static void cr_sticky(CycleResourceRun *run,const CycleResourceSpec *spec) {
    if(!run->policy)return;unsigned at=spec->kind==2?0:spec->count-1;if(!run->functions[at].type)return;
    const XrXirFunctionBinding *b=xr_xir_function_binding(&run->functions[at]);CHECK(b && b==run->observed_binding);
    for(unsigned c=0;c<b->capture_count;++c){const XirCell *cell=(const XirCell *)object_pointer(&b->captures[c]);CHECK(!memcmp(&cell->value,&run->committed[c],sizeof(cell->value)));}
}
static bool cr_finish_selected(CycleResourceRun *run,const CycleResourceSpec *spec,XrXirInstanceResult event) {
    CHECK(event.outcome.status==XR_XIR_CALL_RETURNED);
    if(spec->kind<2){if(!cr_take(run,0,spec->count-1,spec->shape))return false;++run->main_count;return spec->kind?true:cr_wire(run,spec->count);}
    XrXirValue result={0};XrXirCallStatus status=xr_xir_instance_take_result(run->instances[0],&result);cr_objects_observe();if(!cr_status(run,status,XR_XIR_CALL_RETURNED)){CHECK(!result.type && !result.payload);return false;}
    CHECK(result.type==XR_XIR_I64 && result.payload==17);xr_xir_value_drop(&result);return true;
}
static bool cr_control(CycleResourceRun *run,const CycleResourceSpec *spec,XrXirInstanceResult event) {
    cr_policy(run,spec,event);XrXirCallStatus status;
    if(!spec->mode) {
        status=xr_xir_instance_cancel_current(run->instances[0]);CHECK(status==(run->terminal?XR_XIR_CALL_BAD_STATE:XR_XIR_CALL_CANCEL_REQUESTED));run->status=status;
        if(event.outcome.status==XR_XIR_CALL_SUSPENDED)CHECK(xr_xir_instance_resume(run->instances[0],event.epoch,event.outcome.wake)==XR_XIR_CALL_BAD_STATE);
        if(run->terminal){CHECK(xr_xir_call_state(run->instances[0]->call)==XR_XIR_CALL_CONSUMED);XrXirValue empty={0};CHECK(xr_xir_instance_take_result(run->instances[0],&empty)==XR_XIR_CALL_BAD_STATE && !empty.type && !empty.payload);}
        else {unsigned polls=0;XrXirInstanceResult last={0};do {last=cr_tick(run,0);CHECK(++polls<4096);if(!run->complete)return false;}while(last.outcome.status==XR_XIR_CALL_READY);CHECK(last.outcome.status==XR_XIR_CALL_CANCELLED);}
    } else {
        status=spec->mode==1?xr_xir_instance_stop(run->instances[0]):xr_xir_instance_free(run->instances[0]);if(spec->mode==2)run->instances[0]=NULL;run->status=status;cr_objects_observe();
        if(!cr_status(run,status,XR_XIR_CALL_READY))return false;
        if(spec->mode==1 && event.outcome.status==XR_XIR_CALL_SUSPENDED)CHECK(xr_xir_instance_resume(run->instances[0],event.epoch,event.outcome.wake)==XR_XIR_CALL_BAD_STATE);
    }cr_sticky(run,spec);return true;
}
static bool cr_peer(CycleResourceRun *run,const CycleResourceSpec *spec) {
    if(cr_diagnostic)fprintf(stderr,"Cycle diagnose beforePeer work=%llu limit=%llu\n",(unsigned long long)cr_previous[1].work,(unsigned long long)cr_previous[1].work_limit);++run->phase;XrXirValue command={XR_XIR_I64,0,1};XrXirCallStatus status=xr_xir_instance_start_function(run->instances[1],&run->functions[spec->count],&command,1);cr_objects_observe();if(cr_diagnostic)fprintf(stderr,"Cycle diagnose peerStart status=%u work=%llu limit=%llu localWork=%llu\n",status,(unsigned long long)cr_previous[1].work,(unsigned long long)cr_previous[1].work_limit,(unsigned long long)(run->instances[1]->call?run->instances[1]->call->config.admission.work:0));if(!cr_status(run,status,XR_XIR_CALL_READY))return false;
    XrXirInstanceResult event={0};unsigned polls=0;do {event=cr_tick(run,1);CHECK(++polls<4096);if(!run->complete)return false;CHECK(event.outcome.status!=XR_XIR_CALL_SUSPENDED);}while(event.outcome.status==XR_XIR_CALL_READY);
    CHECK(event.outcome.status==XR_XIR_CALL_RETURNED);XrXirValue result={0};status=xr_xir_instance_take_result(run->instances[1],&result);cr_objects_observe();if(!cr_status(run,status,XR_XIR_CALL_RETURNED)){CHECK(!result.type && !result.payload);return false;}CHECK(result.type==XR_XIR_I64 && result.payload==42);xr_xir_value_drop(&result);cr_sticky(run,spec);return true;
}
static void cr_drive(CycleResourceRun *run,const CycleResourceSpec *spec) {
    run->count=spec->count;if(!cr_make(run,1,spec->count,1-spec->shape))return;
    unsigned initial=spec->kind==2?spec->count:spec->count-1;
    for(unsigned i=0;i<initial;++i){if(!cr_make(run,0,i,spec->shape))return;++run->main_count;}
    if(!cr_wire(run,initial))return;
    if(initial && (!cr_reject(run,0,spec->count,0) || !cr_reject(run,spec->count,0,1)))return;
    if(spec->kind==2){if(!cr_start_break(run))return;}else if(!cr_start_make(run,0,spec->shape,spec->kind))return;
    run->selected=true;XrXirInstanceResult event={.outcome={.status=XR_XIR_CALL_READY}};run->polls=0;
    while(run->polls<spec->prefix) {
        if(event.outcome.status==XR_XIR_CALL_SUSPENDED)cr_resume(run,0,event);event=cr_tick(run,0);++run->polls;CHECK(run->polls<4096);if(!run->complete)return;
        if(event.outcome.status==XR_XIR_CALL_RETURNED)break;
    }
    if(spec->prefix==UINT32_MAX){CHECK(event.outcome.status==XR_XIR_CALL_RETURNED);if(!cr_finish_selected(run,spec,event))return;return;}
    CHECK(run->polls==spec->prefix);
    if(event.outcome.status==XR_XIR_CALL_RETURNED && !cr_finish_selected(run,spec,event))return;
    if(cr_control(run,spec,event))(void)cr_peer(run,spec);
}
static void cr_retire(CycleResourceRun *run,const CycleResourceSpec *spec) {
    for(unsigned n=0;n<2;++n){unsigned at=n?1-spec->close:spec->close;if(!run->instances[at])continue;XrXirCallStatus status=xr_xir_instance_free(run->instances[at]);run->instances[at]=NULL;cr_objects_observe();if(status==run->fault)run->complete=false;else CHECK(status==XR_XIR_CALL_READY);}
    cr_sticky(run,spec);size_t attempts=runtime_attempts;
    for(unsigned i=0;i<=spec->count;++i)xr_xir_value_drop(&run->functions[i]);
    for(unsigned n=0;n<spec->count;++n){unsigned at=spec->drop?spec->count-1-n:n;if(run->aliases[at].type){CHECK(xr_xir_value_valid(&run->aliases[at]));const XrXirFunctionBinding *b=xr_xir_function_binding(&run->aliases[at]);CHECK(b && ((const FunctionGate *)b->owner)->domain==cr_borrowed[0] && !((const FunctionGate *)b->owner)->instance);for(unsigned c=0;c<b->capture_count;++c){XrXirValue edge={0};CHECK(xr_xir_cell_read(&b->captures[c],&edge)==XR_XIR_VALUE_OK && xr_xir_value_valid(&edge));xr_xir_value_drop(&edge);}}xr_xir_value_drop(&run->aliases[at]);cr_cost();}
    if(run->aliases[spec->count].type){CHECK(!run->fixture.releases && xr_xir_value_valid(&run->aliases[spec->count]));xr_xir_value_drop(&run->aliases[spec->count]);}
    CHECK(attempts==runtime_attempts && run->fixture.releases==1 && !run->fixture.vm && !run->fixture.entries);
}
static CycleResourceResult cr_operation(const CycleResourceSpec *spec,size_t failure,const XrXirInstanceConfig *limits,unsigned limited_owner) {
    CHECK(spec->backend<4 && spec->shape<2 && spec->count && spec->count<=16 && spec->kind<3 && spec->mode<3 && spec->close<2 && spec->drop<2 && limited_owner<2);
    CHECK(!runtime_live && !effects_compile_live && !cr_borrowed[0] && !cr_borrowed[1]);runtime_fail_at=SIZE_MAX;
    memset(cr_final,0,sizeof(cr_final));memset(cr_previous,0,sizeof(cr_previous));memset(cr_disposals,0,sizeof(cr_disposals));memset(cr_objects,0,sizeof(cr_objects));cr_object_count=cr_object_frees=0;
    CycleResourceRun run={.complete=true,.fault=limits?XR_XIR_CALL_LIMIT:XR_XIR_CALL_OOM};runtime_attempts=0;XrXirProgram *program=cycle_program(&run.fixture,spec->backend);CycleResourceResult result={.setup=runtime_attempts};CHECK(!runtime_live && !runtime_bytes);
    XrXirInstanceConfig config[2];for(unsigned i=0;i<2;++i)CHECK(xr_xir_instance_config_init(&config[i],sizeof(config[i]))==XR_XIR_CALL_READY);if(limits)config[limited_owner]=*limits;
    runtime_attempts=0;runtime_fail_at=failure;
    for(unsigned i=0;i<2;++i){XrXirCallStatus status=xr_xir_instance_new(program,&config[i],&run.instances[i]);if(!cr_status(&run,status,XR_XIR_CALL_READY)){CHECK(!run.instances[i]);break;}run.published[i]=true;cr_borrowed[i]=run.instances[i]->domain;CHECK(!i || cr_borrowed[0]!=cr_borrowed[1]);cr_objects_observe();}
    xr_xir_compile_program_drop(program);if(run.complete)cr_drive(&run,spec);cr_retire(&run,spec);
    for(unsigned i=0;i<2;++i){CHECK(!cr_borrowed[i] && cr_disposals[i]==(unsigned)run.published[i]);result.cost[i]=cr_final[i];result.published+=(unsigned)run.published[i];result.disposals+=cr_disposals[i];CHECK(result.cost[i].requested_bytes<=config[i].requested_value_limit && result.cost[i].requested_call_bytes<=config[i].requested_call_limit && result.cost[i].work<=config[i].work_limit);}
    CHECK(cr_object_frees==cr_object_count);result.observed=cr_object_count;result.frees=cr_object_frees;result.sites=runtime_attempts;result.complete=run.complete;result.hit=failure!=SIZE_MAX && runtime_attempts>failure;result.polls=run.polls;result.phase=run.phase;result.mask=run.mask;result.terminal=run.terminal;result.status=run.status;memcpy(result.before_edges,run.before_edges,sizeof(result.before_edges));
    runtime_fail_at=SIZE_MAX;CHECK(!runtime_live && !runtime_bytes && !effects_compile_live && !effects_compile_bytes);return result;
}
static CycleResourceResult cr_baseline(const CycleResourceSpec *spec) {
    CycleResourceResult r=cr_operation(spec,SIZE_MAX,NULL,0);CHECK(r.complete && r.published==2 && r.disposals==2 && !r.setup && r.sites && r.sites<20000 && r.observed==r.frees);
    printf("Cycle resources backend=%u shape=%u ring=%u kind=%u mode=%u prefix=%u close=%u drop=%u sites=%zu polls=%u phase=%u mask=%u terminal=%u status=%u edge0=%u edge1=%u value0=%llu call0=%llu work0=%llu value1=%llu call1=%llu work1=%llu observed=%u originalDomains2 lastFree2 codeLease1 noAllocateDrop physical0/0\n",spec->backend,spec->shape,spec->count,spec->kind,spec->mode,spec->prefix,spec->close,spec->drop,r.sites,r.polls,r.phase,r.mask,r.terminal,r.status,r.before_edges[0],r.before_edges[1],(unsigned long long)r.cost[0].requested_bytes,(unsigned long long)r.cost[0].requested_call_bytes,(unsigned long long)r.cost[0].work,(unsigned long long)r.cost[1].requested_bytes,(unsigned long long)r.cost[1].requested_call_bytes,(unsigned long long)r.cost[1].work,r.observed);return r;
}
static void cr_axes(const CycleResourceSpec *spec,const CycleResourceResult *normal) {
    for(unsigned owner=0;owner<2;++owner){const uint64_t fees[]={normal->cost[owner].requested_bytes,normal->cost[owner].requested_call_bytes,normal->cost[owner].work};
        for(unsigned axis=0;axis<3;++axis)for(unsigned minus=0;minus<2;++minus){CHECK(fees[axis]>1);XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);uint64_t cap=fees[axis]-minus;if(!axis)config.requested_value_limit=cap;else if(axis==1)config.requested_call_limit=cap;else config.work_limit=cap;CycleResourceResult r=cr_operation(spec,SIZE_MAX,&config,owner);if(r.complete!=!minus)fprintf(stderr,"Cycle axis owner=%u axis=%u minus=%u complete=%u phase=%u\n",owner,axis,minus,r.complete,r.phase);CHECK(r.complete==!minus);if(!minus){CHECK(r.phase==normal->phase && r.polls==normal->polls && r.mask==normal->mask && r.terminal==normal->terminal && r.status==normal->status && r.published==2 && r.disposals==2 && r.observed==normal->observed && !memcmp(r.before_edges,normal->before_edges,sizeof(r.before_edges)));for(unsigned i=0;i<2;++i)CHECK(r.cost[i].requested_bytes==normal->cost[i].requested_bytes && r.cost[i].requested_call_bytes==normal->cost[i].requested_call_bytes && r.cost[i].work==normal->cost[i].work);}printf("Cycle axis backend=%u shape=%u ring=%u kind=%u mode=%u prefix=%u close=%u drop=%u owner=%u axis=%u minus=%u complete=%u physical0/0\n",spec->backend,spec->shape,spec->count,spec->kind,spec->mode,spec->prefix,spec->close,spec->drop,owner,axis,minus,r.complete);}
    }
}
#endif // XIR_MIXED_CYCLE_BATCH_RESOURCES_H
