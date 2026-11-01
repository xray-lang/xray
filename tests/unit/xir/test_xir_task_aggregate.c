/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_task_aggregate.c - Publication order and early-return ownership
 *
 * KEY CONCEPT:
 *   The caller progresses before a cancelled member finishes cleanup, while
 *   the original lexical frontier still owns and drains that member.
 */
#include "xir/xxir_program_internal.h"
#include "xir/xxir_task_internal.h"
#include "base/xresource_limits.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do{if(!(c)){fprintf(stderr,"aggregate %d: %s\n",__LINE__,#c);exit(1);}}while(0)
static XrXirCall *aggregate_cancel_parent;
static size_t aggregate_cancel_at;
static void aggregate_after_allocate(void);
#define XIR_RUNTIME_AFTER_ALLOCATE(pointer,bytes) aggregate_after_allocate()
#include "xir_runtime_allocations.h"
#include "xir/xxir_task_budget.c"
#include "xir/xxir_task.c"
#include "base/xcompile_resources.c"
#include "xir_native_metadata_fixture.h"
static void aggregate_after_allocate(void) {
    if(aggregate_cancel_parent&&runtime_attempts==aggregate_cancel_at)
        CHECK(xr_xir_call_request_cancel(aggregate_cancel_parent)==XR_XIR_CALL_CANCEL_REQUESTED);
}
enum { AGG_ERROR=256,AGG_TASK,AGG_SCOPE,AGG_INPUT,AGG_VALUES,AGG_ALL };
typedef struct AggregateWitness {
    XrXirTaskExecutor *executor;
    XrXirValue group,members[2],array,aggregate;
    uint32_t mode,cleanup_calls,refusals;
    bool progressed,release_cleanup,held_cleanup;
} AggregateWitness;
typedef struct AggregateState {uint32_t phase;bool cleanup_waiting,cleanup_done;} AggregateState;
typedef struct AggregateWorkerState {uint32_t phase;XrXirValue error;} AggregateWorkerState;
static XrXirAction aggregate_worker(XrXirCallView *view) {
    AggregateWitness *w=(AggregateWitness *)view->environment;AggregateWorkerState *state=view->state;
    bool slow=view->arguments[0].payload==20;
    if(view->phase==XR_XIR_CALL_EXIT) {
        if(slow&&(w->mode==1||w->mode==2||w->mode==3)&&!w->release_cleanup) {
            CHECK(view->activation->cancel_requested);w->held_cleanup=true;
            return (XrXirAction){.kind=XR_XIR_ACTION_CONTINUE};
        }
        return (XrXirAction){.kind=XR_XIR_ACTION_EXIT_DONE};
    }
    if(slow&&state->phase++<4)return (XrXirAction){.kind=XR_XIR_ACTION_CONTINUE};
    if(!slow&&(w->mode==2||w->mode==3)) {
        XrXirValue field={XR_XIR_I64,0,42};
        CHECK(xr_xir_enum_new((XrXirType)AGG_ERROR,0,&field,1,xr_xir_call_admission(view),&state->error)==XR_XIR_VALUE_OK);
        return (XrXirAction){.kind=XR_XIR_ACTION_THROW,.value=state->error};
    }
    return (XrXirAction){.kind=XR_XIR_ACTION_RETURN,.value=view->arguments[0]};
}
static void aggregate_worker_release(XrXirCallView *view,XrXirCallStatus reason) {
    (void)reason;AggregateWorkerState *state=view->state;xr_xir_value_drop(&state->error);
}
static void aggregate_begin(AggregateWitness *w,XrXirCallView *view) {
    XrXirValueAdmission *admission=xr_xir_call_admission(view);
    XrXirValue inputs[]={w->members[0],w->members[1],w->members[0]};
    size_t count=w->mode==4?0:3;
    CHECK(xr_xir_array_new((XrXirType)AGG_INPUT,inputs,count,admission,&w->array)==XR_XIR_VALUE_OK);
    XirTaskAggregateKind kind=w->mode==1||w->mode==2||w->mode==5?XIR_TASK_AGGREGATE_FIRST:XIR_TASK_AGGREGATE_ALL;
    XrXirType result=kind==XIR_TASK_AGGREGATE_FIRST?(XrXirType)AGG_TASK:(XrXirType)AGG_ALL;
    XrXirValue refused={0};uint64_t reserved=w->executor->pending_terminals;
    CHECK(xir_task_aggregate_begin(w->executor,view,&w->array,(XrXirType)AGG_SCOPE,kind,&refused)==XR_XIR_CALL_BAD_ARGUMENT);
    CHECK(!refused.type&&w->executor->pending_terminals==reserved);++w->refusals;
    if(w->mode==7)w->executor->publication_order=UINT64_MAX-reserved-1;
    if(w->mode==9) {
        size_t blocks=runtime_live,bytes=runtime_bytes;XirTaskGroup *groups=w->executor->groups;
        aggregate_cancel_parent=view->activation;aggregate_cancel_at=runtime_attempts+3;
        CHECK(xir_task_aggregate_begin(w->executor,view,&w->array,result,kind,&w->aggregate)==XR_XIR_CALL_CANCELLED);
        aggregate_cancel_parent=NULL;
        CHECK(!w->aggregate.type&&w->executor->groups==groups&&w->executor->pending_terminals==reserved);
        CHECK(runtime_live==blocks&&runtime_bytes==bytes);return;
    } else if(w->mode==8) {
        size_t failures=0;
        for(size_t offset=0;offset<16;++offset) {
            size_t blocks=runtime_live,bytes=runtime_bytes;uint64_t identity=w->executor->next_identity;
            XirTaskGroup *groups=w->executor->groups;runtime_fail_at=runtime_attempts+offset;
            XrXirCallStatus status=xir_task_aggregate_begin(w->executor,view,&w->array,result,kind,&w->aggregate);
            runtime_fail_at=SIZE_MAX;
            if(status==XR_XIR_CALL_READY)break;
            CHECK(status==XR_XIR_CALL_OOM&&!w->aggregate.type&&runtime_live==blocks&&runtime_bytes==bytes);
            CHECK(w->executor->groups==groups&&w->executor->next_identity==identity&&w->executor->pending_terminals==reserved);
            for(size_t a=0;a<2;++a)CHECK(((XirTask *)(uintptr_t)w->members[a].payload)->subscriptions->next_subscription==NULL);
            ++failures;
        }
        CHECK(failures>=3&&w->aggregate.type);
    } else CHECK(xir_task_aggregate_begin(w->executor,view,&w->array,result,kind,&w->aggregate)==XR_XIR_CALL_READY);
    if(w->mode==6)CHECK(xr_xir_task_executor_cancel_task(w->executor,view,&w->aggregate)==XR_XIR_CALL_READY);
    if(w->mode==4) {
        uint64_t identity=w->executor->next_identity;
        CHECK(xir_task_aggregate_begin(w->executor,view,&w->array,(XrXirType)AGG_TASK,
            XIR_TASK_AGGREGATE_FIRST,&refused)==XR_XIR_CALL_UNSUPPORTED);
        CHECK(!refused.type&&w->executor->next_identity==identity);++w->refusals;
    }
    if(w->mode==7) {
        uint64_t before=w->executor->publication_order,stamp=0;
        CHECK(xir_task_scope_failure_order(w->executor,&stamp)==XR_XIR_CALL_LIMIT&&!stamp);
        XrXirValue argument={XR_XIR_I64,0,42},extra={0};const XrXirCallRequest request={.entry=2,.arguments=&argument,.count=1};
        CHECK(xr_xir_task_executor_spawn_from_view(w->executor,view,(XrXirType)AGG_TASK,&request,&extra)==XR_XIR_CALL_LIMIT);
        CHECK(!extra.type&&w->executor->publication_order==before);++w->refusals;
    }
}
static XrXirAction aggregate_root(XrXirCallView *view) {
    AggregateWitness *w=(AggregateWitness *)view->environment;AggregateState *state=view->state;
    if(view->phase==XR_XIR_CALL_EXIT) {
        if(state->cleanup_done)return (XrXirAction){.kind=XR_XIR_ACTION_EXIT_DONE};
        if(!state->cleanup_waiting) {
            state->cleanup_waiting=true;
            return (XrXirAction){.kind=XR_XIR_ACTION_CALL,.callee=3,.flags=XR_XIR_ACTION_CLEANUP};
        }
        CHECK(view->inbox.status==XR_XIR_CALL_RETURNED);
        CHECK(xr_xir_call_cleanup_frontier(view,0)==XR_XIR_CALL_READY);state->cleanup_done=true;
        return (XrXirAction){.kind=XR_XIR_ACTION_EXIT_DONE};
    }
    if(!state->phase++) {
        CHECK(xr_xir_call_cleanup_frontier(view,1)==XR_XIR_CALL_READY);
        CHECK(xir_task_scope_begin(w->executor,view,(XrXirType)AGG_SCOPE,0,&w->group)==XR_XIR_CALL_READY);
        for(size_t a=0;a<2;++a) {
            XrXirValue argument={XR_XIR_I64,0,a?22:20};const XrXirCallRequest request={.entry=2,.arguments=&argument,.count=1};
            CHECK(xir_task_spawn_scoped(w->executor,view,&w->group,(XrXirType)AGG_TASK,&request,&w->members[a])==XR_XIR_CALL_READY);
        }
        if(w->mode==5)return (XrXirAction){.kind=XR_XIR_ACTION_CONTINUE};
        aggregate_begin(w,view);
        if(w->mode==9||w->mode==6)return (XrXirAction){.kind=XR_XIR_ACTION_CONTINUE};
        return (XrXirAction){.kind=XR_XIR_ACTION_AWAIT_TASK,.value=w->aggregate};
    }
    if(w->mode==5&&!w->aggregate.type) {
        for(size_t a=0;a<2;++a)if(atomic_load_explicit(&((XirTask *)(uintptr_t)w->members[a].payload)->state,
            memory_order_acquire)!=XIR_TASK_TERMINAL)return (XrXirAction){.kind=XR_XIR_ACTION_CONTINUE};
        aggregate_begin(w,view);
        return (XrXirAction){.kind=XR_XIR_ACTION_AWAIT_TASK,.value=w->aggregate};
    }
    if(w->mode==6) {
        XrXirCallResult result={0};
        CHECK(xr_xir_task_copy_outcome(&w->aggregate,&result)==XR_XIR_CALL_CANCELLED);
        xr_xir_call_result_drop(&result);
        CHECK(xr_xir_task_copy_outcome(&w->aggregate,&result)==XR_XIR_CALL_CANCELLED);
        xr_xir_call_result_drop(&result);
        return (XrXirAction){.kind=XR_XIR_ACTION_RETURN,.value={XR_XIR_I64,0,42}};
    }
    XrXirCallStatus expected=w->mode==2||w->mode==3?XR_XIR_CALL_THROWN:
        w->mode==6?XR_XIR_CALL_CANCELLED:XR_XIR_CALL_RETURNED;
    CHECK(view->inbox.status==expected);
    XrXirValueAdmission *admission=xr_xir_call_admission(view);
    if(w->mode==2||w->mode==3||w->mode==6) {
        if(w->mode!=6) {
            XrXirValue field={0};CHECK(xr_xir_enum_get(&view->inbox.value,0,0,admission,&field)==XR_XIR_VALUE_OK);
            CHECK(field.payload==42);xr_xir_value_drop(&field);
        }
        if(w->mode!=6)w->progressed=true;
    } else if(w->mode==1||w->mode==5) {
        CHECK(view->inbox.value.type==XR_XIR_I64&&view->inbox.value.payload==22);
        if(w->mode==1) {
            XirTask *slow=(XirTask *)(uintptr_t)w->members[0].payload;
            CHECK(slow->call&&slow->call->cancel_requested&&!w->release_cleanup);
            CHECK(((XirTask *)(uintptr_t)w->group.payload)->aggregation->pending);w->progressed=true;
        }
    } else {
        int64_t length=-1;CHECK(xr_xir_array_len(&view->inbox.value,admission,&length)==XR_XIR_VALUE_OK);
        CHECK(length==(w->mode==4?0:3));
        for(int64_t a=0;a<length;++a) {
            XrXirValue value={0};XrXirFaultDetail fault={0};
            CHECK(xr_xir_array_get(&view->inbox.value,a,admission,&value,&fault)==XR_XIR_VALUE_OK);
            CHECK(value.type==XR_XIR_I64&&value.payload==(a==1?22:20));xr_xir_value_drop(&value);
        }
    }
    XrXirCallResult first={0},again={0};
    CHECK(xr_xir_task_copy_outcome(&w->aggregate,&first)==expected);
    uint64_t order=((XirTask *)(uintptr_t)w->aggregate.payload)->terminal_order;
    CHECK(xr_xir_task_copy_outcome(&w->aggregate,&again)==expected);
    CHECK(order==((XirTask *)(uintptr_t)w->aggregate.payload)->terminal_order);
    xr_xir_call_result_drop(&first);xr_xir_call_result_drop(&again);
    return (XrXirAction){.kind=XR_XIR_ACTION_RETURN,.value={XR_XIR_I64,0,42}};
}
static XrXirAction aggregate_cleanup(XrXirCallView *view) {
    AggregateWitness *w=(AggregateWitness *)view->environment;++w->cleanup_calls;
    CHECK(!((XirTask *)(uintptr_t)w->group.payload)->aggregation->pending);
    return (XrXirAction){.kind=XR_XIR_ACTION_RETURN};
}
static XrXirAction aggregate_init(XrXirCallView *view) {
    (void)view;return (XrXirAction){.kind=XR_XIR_ACTION_RETURN};
}
static XrXirStatus aggregate_metadata(const XrXirCompileContext *context,XrXirProgramSpec *spec,
    XrXirArtifact **output,bool errors) {
    XrXirFunction functions[5]={0};XrXirInstruction code[5][2]={0};
    const XrXirBlock block={0,2,0,0};
    for(uint32_t f=0;f<5;++f) {
        const XrXirCallEntry *entry=spec->entries+f;
        code[f][0]=(XrXirInstruction){XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},0,{0}};
        code[f][1]=(XrXirInstruction){XR_XIR_MATCH_FAIL,XR_XIR_UNIT,{0},{0},0,{0}};
        functions[f]=(XrXirFunction){"entry",5,entry->parameters,entry->parameter_count,entry->result,
            &block,1,code[f],2,NULL,0,errors&&f==2?(XrXirType)AGG_ERROR:XR_XIR_NEVER,0,NULL};
    }
    XrXirModule module={XR_XIR_BUILT,functions,5,spec->declarations,NULL,spec->types,NULL,XR_XIR_PROGRAM,NULL};
    XrXirArtifact *checked=NULL,*closed=NULL;XrXirStatus status=xir_fixture_check(context,&module,&checked,NULL);
    if(status==XR_XIR_OK)status=xr_xir_compile_specialize(checked,&closed,NULL,NULL);
    if(status==XR_XIR_OK)status=xr_xir_compile_lower(closed,&spec->target,NULL,output,NULL);
    xr_xir_compile_artifact_free(closed);xr_xir_compile_artifact_free(checked);
    if(status==XR_XIR_OK) {
        const XrXirModule *lowered=xr_xir_compile_artifact_module(*output);
        spec->types=lowered->types;spec->declarations=lowered->declarations;
    }
    return status;
}
static void aggregate_case(uint32_t mode) {
    NativeFixtureOwner compiler={0};CHECK(native_fixture_owner_new(&compiler)==XR_XIR_OK);
    XrCompileResourceStats baseline=native_fixture_stats(&compiler.context);AggregateWitness witness={.mode=mode};
    const XrXirType parameter=XR_XIR_I64;
    bool errors=mode==2||mode==3;
    const XrXirCallEntry entries[]={
        {.abi_version=XR_XIR_CALL_ABI_VERSION,.result=XR_XIR_UNIT,.resume=aggregate_init},
        {.abi_version=XR_XIR_CALL_ABI_VERSION,.result=XR_XIR_I64,.state_bytes=sizeof(AggregateState),
         .flags=XR_XIR_ENTRY_EXIT,.resume=aggregate_root,.environment=&witness},
        {.abi_version=XR_XIR_CALL_ABI_VERSION,.parameters=&parameter,.parameter_count=1,.result=XR_XIR_I64,
         .state_bytes=sizeof(AggregateWorkerState),.flags=XR_XIR_ENTRY_EXIT,.resume=aggregate_worker,
         .release=aggregate_worker_release,.environment=&witness},
        {.abi_version=XR_XIR_CALL_ABI_VERSION,.result=XR_XIR_UNIT,.cleanup_owner=2,.resume=aggregate_cleanup,.environment=&witness},
        {.abi_version=XR_XIR_CALL_ABI_VERSION,.result=XR_XIR_UNIT,.cleanup_owner=3,.resume=aggregate_init}};
    XrXirFunctionIdentity identities[5]={{0},{.exported=1},{.exported=1},{.cleanup_owner=2},{.cleanup_owner=3}};
    const XrXirSourceModule source={"aggregate",9,NULL,0,0,0,XR_XIR_MODULE_FRESH_STORAGE};
    const XrXirDeclarations declarations={&source,1,identities,NULL,0,NULL,0,0,1,NULL};
    const XrXirNominalField field={{"value",5},XR_XIR_I64,0};const XrXirType projection=XR_XIR_I64;
    const XrXirNominalVariant variant={{"Failed",6},0,1};
    const XrXirNominalDeclaration nominal={.module={"aggregate",9},.name={"Failure",7},
        .kind=XR_XIR_NOMINAL_ENUM,.fields=&field,.field_count=1,.variants=&variant,.variant_count=1};
    const XrXirNominalTable table={&nominal,1,NULL};
    const XrXirTypeNode nodes[]={
        {.kind=XR_XIR_TYPE_NOMINAL,.nominal={0,NULL,0,&projection,1}},
        {.kind=XR_XIR_TYPE_TASK,.element=XR_XIR_I64,.result=errors?(XrXirType)AGG_ERROR:XR_XIR_NEVER},
        {.kind=XR_XIR_TYPE_TASK,.element=XR_XIR_UNIT,.result=errors?(XrXirType)AGG_ERROR:XR_XIR_NEVER},
        {.kind=XR_XIR_TYPE_ARRAY,.element=(XrXirType)AGG_TASK},
        {.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_I64},
        {.kind=XR_XIR_TYPE_TASK,.element=(XrXirType)AGG_VALUES,.result=errors?(XrXirType)AGG_ERROR:XR_XIR_NEVER}};
    const XrXirTypes types={nodes,6,&table,NULL};
    XrXirProgramSpec spec={XR_XIR_PROGRAM_ABI_VERSION,{XR_XIR_ARCH_LE64,XR_XIR_VALUE_ABI_VERSION},
        entries,5,&declarations,{0},&types,{0}};
    XrXirArtifact *artifact=NULL;CHECK(aggregate_metadata(&compiler.context,&spec,&artifact,errors)==XR_XIR_OK);
    XrXirProgramInput *input=NULL;CHECK(xir_program_input_take(&compiler.context,&artifact,&input)==XR_XIR_OK&&!artifact);
    XrXirProgram *program=NULL;XrXirStatus received=xir_program_receive(&compiler.context,&spec,&input,NULL,&program);
    if(received!=XR_XIR_OK)fprintf(stderr,"aggregate program receive status%u\n",received);
    CHECK(received==XR_XIR_OK&&!input);
    XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(1048576,&domain)==XR_XIR_VALUE_OK);
    const XrXirDomainBudgetControls controls={2097152,2097152,XR_RESOURCE_SANDBOX_RUNTIME_WORK,1048576,1048576};
    CHECK(xr_xir_domain_budget_bind(domain,&controls)==XR_XIR_VALUE_OK);
    XrXirCallBudget budget={.byte_limit=1048576,.requested_limit=2097152,.resume_limit=1000000,.work_domain=domain};
    const XrXirTaskExecutorConfig execution={.program=program,.domain=domain,.task_arena=program->arena,
        .call_limit=1048576,.poll_limit=1000000,.requested_value_limit=2097152,.requested_call_limit=2097152,
        .work_limit=XR_RESOURCE_SANDBOX_RUNTIME_WORK,.depth_limit=64,.budget=&budget};
    CHECK(xr_xir_task_executor_new(&execution,&witness.executor)==XR_XIR_CALL_READY);
    XrXirCallConfig config;CHECK(xr_xir_call_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirCallAccounting accounting={0};config.accounting=&accounting;
    config.entries=program->entries;config.entry_count=program->entry_count;config.byte_limit=1048576;config.poll_limit=1000000;
    config.admission=(XrXirValueAdmission){program->arena,domain,NULL,NULL,1000000,1048576};
    const XrXirCallRequest request={.entry=1};XrXirCall *root=NULL;
    CHECK(xr_xir_call_new_budgeted(&config,&request,&budget,&root)==XR_XIR_CALL_READY);
    CHECK(xr_xir_task_executor_attach_root(witness.executor,root,1)==XR_XIR_CALL_READY);
    XrXirCallResult result={0};unsigned polls=0;
    do {
        CHECK(++polls<128);result=xr_xir_task_executor_poll_root(witness.executor,1);
        if((mode==1||errors)&&witness.progressed&&witness.held_cleanup&&!witness.release_cleanup&&
            root->top->scope_draining) {
            CHECK(result.status==XR_XIR_CALL_READY);
            CHECK(root->top->scope_draining&&xr_xir_call_state(root)==XR_XIR_CALL_SUSPENDED);
            CHECK(!witness.cleanup_calls);witness.release_cleanup=true;
        }
    }while(result.status==XR_XIR_CALL_READY);
    fprintf(stderr,"aggregate mode%u status%u polls%u refusals%u\n",mode,result.status,polls,witness.refusals);
    CHECK(result.status==(errors?XR_XIR_CALL_THROWN:mode==9?XR_XIR_CALL_CANCELLED:XR_XIR_CALL_RETURNED));
    CHECK(witness.cleanup_calls==1);
    if(!errors&&mode!=9)CHECK(result.value.payload==42);
    CHECK(!witness.executor->pending_terminals);
    if(mode==7)CHECK(witness.executor->publication_order==UINT64_MAX);
    if(mode==1||errors)CHECK(witness.progressed&&witness.held_cleanup&&witness.release_cleanup);
    CHECK(xr_xir_task_executor_free(witness.executor,NULL)==XR_XIR_CALL_READY);
    CHECK(xr_xir_call_free(root)==XR_XIR_CALL_READY);
    xr_xir_value_drop(&witness.aggregate);xr_xir_value_drop(&witness.array);xr_xir_value_drop(&witness.group);
    for(size_t a=0;a<2;++a)xr_xir_value_drop(&witness.members[a]);
    CHECK(!budget.live_bytes);xr_xir_domain_drop(domain);xr_xir_compile_program_drop(program);
    CHECK(native_fixture_stats(&compiler.context).held_bytes==baseline.held_bytes);native_fixture_owner_free(&compiler);
    CHECK(!runtime_live&&!runtime_bytes);
}
int main(void) {
    fprintf(stderr,"aggregate private sizes task%zu executor%zu group%zu\n",sizeof(XirTask),sizeof(XrXirTaskExecutor),sizeof(XirTaskGroup));
    for(uint32_t mode=0;mode<10;++mode)aggregate_case(mode);return 0;
}
