/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_task_scope.c - Real lexical registration and terminal-driven join
 *
 * KEY CONCEPT:
 *   One root parks once on an aggregate while the existing FIFO drives all
 *   accepted members. Independent handles keep their sticky outcomes.
 */
#include "xir/xxir_program_internal.h"
#include "xir/xxir_task_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do{if(!(c)){fprintf(stderr,"scope %d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "xir_runtime_allocations.h"
#include "xir/xxir_task_budget.c"
#include "xir/xxir_task.c"
#include "base/xcompile_resources.c"
#include "xir_native_metadata_fixture.h"
typedef struct ScopeWitness {
    XrXirTaskExecutor *executor;
    XrXirValue group,members[2],inner,inner_member,cleanup_argument;
    uint32_t mode,root_calls,child_calls,cleanup_calls;
    bool user_panic;
} ScopeWitness;
typedef struct ScopeState {uint32_t phase,cleanup_step;bool cleanup_waiting,cleanup_done,exit_delay,body_raised;} ScopeState;
static XrXirAction scope_worker(XrXirCallView *view) {
    ScopeWitness *witness=(ScopeWitness *)view->environment;++witness->child_calls;
    uint32_t *phase=view->state;
    if(view->phase==XR_XIR_CALL_EXIT) {
        if(witness->mode==9&&view->arguments[0].payload<0&&!witness->user_panic)
            return (XrXirAction){.kind=XR_XIR_ACTION_CONTINUE};
        return (XrXirAction){.kind=XR_XIR_ACTION_EXIT_DONE};
    }
    if(view->arguments[0].payload<0)return xr_xir_call_bounds(-7,2);
    if(witness->mode==2&&!(*phase)++)return (XrXirAction){.kind=XR_XIR_ACTION_SUSPEND};
    if((*phase)++<2)return (XrXirAction){.kind=XR_XIR_ACTION_CONTINUE};
    return (XrXirAction){.kind=XR_XIR_ACTION_RETURN,.value={XR_XIR_I64,0,view->arguments[0].payload}};
}
static XrXirAction scope_root(XrXirCallView *view) {
    ScopeWitness *witness=(ScopeWitness *)view->environment;++witness->root_calls;
    ScopeState *state=view->state;
    if(view->phase==XR_XIR_CALL_EXIT) {
        if(witness->mode==9) {
            switch(state->cleanup_step++) {
            case 0:return (XrXirAction){.kind=XR_XIR_ACTION_CALL,.callee=5,.flags=XR_XIR_ACTION_CLEANUP};
            case 1:
                CHECK(view->inbox.status==XR_XIR_CALL_RETURNED);
                CHECK(xr_xir_call_cleanup_frontier(view,3)==XR_XIR_CALL_READY);
                witness->cleanup_argument=(XrXirValue){XR_XIR_I64,0,42};
                return (XrXirAction){.kind=XR_XIR_ACTION_CALL,.callee=6,
                    .arguments=&witness->cleanup_argument,.argument_count=1,.flags=XR_XIR_ACTION_CLEANUP};
            case 2:
                CHECK(view->inbox.status==XR_XIR_CALL_BOUNDS);
                CHECK(xr_xir_call_cleanup_frontier(view,1)==XR_XIR_CALL_READY);
                return (XrXirAction){.kind=XR_XIR_ACTION_CONTINUE};
            case 3:return (XrXirAction){.kind=XR_XIR_ACTION_CALL,.callee=3,.flags=XR_XIR_ACTION_CLEANUP};
            default:
                CHECK(view->inbox.status==XR_XIR_CALL_RETURNED);
                CHECK(xr_xir_call_cleanup_frontier(view,0)==XR_XIR_CALL_READY);
                return (XrXirAction){.kind=XR_XIR_ACTION_EXIT_DONE};
            }
        }
        if(state->cleanup_done)return (XrXirAction){.kind=XR_XIR_ACTION_EXIT_DONE};
        if((witness->mode==6||witness->mode==7)&&!state->exit_delay) {
            state->exit_delay=true;return (XrXirAction){.kind=XR_XIR_ACTION_CONTINUE};
        }
        if(!state->cleanup_waiting) {
            state->cleanup_waiting=true;
            return (XrXirAction){.kind=XR_XIR_ACTION_CALL,.callee=3,.flags=XR_XIR_ACTION_CLEANUP};
        }
        CHECK(view->inbox.status==XR_XIR_CALL_RETURNED&&!view->inbox.value.type);
        CHECK(xr_xir_call_cleanup_frontier(view,0)==XR_XIR_CALL_READY);
        state->cleanup_done=true;
        return (XrXirAction){.kind=XR_XIR_ACTION_EXIT_DONE};
    }
    uint32_t phase=state->phase++;
    if(!phase) {
        CHECK(xr_xir_call_cleanup_frontier(view,1)==XR_XIR_CALL_READY);
        CHECK(xir_task_scope_begin(witness->executor,view,(XrXirType)257,1,&witness->group)==XR_XIR_CALL_BAD_ARGUMENT);
        if(!witness->mode) {
            size_t blocks=runtime_live,bytes=runtime_bytes;
            runtime_fail_at=runtime_attempts+1;
            CHECK(xir_task_scope_begin(witness->executor,view,(XrXirType)257,0,&witness->group)==XR_XIR_CALL_OOM);
            runtime_fail_at=SIZE_MAX;
            CHECK(!witness->group.type&&!witness->executor->groups&&runtime_live==blocks&&runtime_bytes==bytes);
        }
        CHECK(xir_task_scope_begin(witness->executor,view,(XrXirType)257,0,&witness->group)==XR_XIR_CALL_READY);
        XrXirCallView forged=*view;XrXirValue refused={0};
        CHECK(xir_task_scope_begin(witness->executor,&forged,(XrXirType)257,0,&refused)!=XR_XIR_CALL_READY&&!refused.type);
        for(uint32_t i=0;i<2;++i) {
            bool panic=(witness->mode==2||witness->mode==6||witness->mode==7||witness->mode==9)&&!i;
            XrXirValue argument={XR_XIR_I64,0,panic?-1:20+(int64_t)i*2};
            const XrXirCallRequest request={.entry=2,.arguments=&argument,.count=1};
            if(!witness->mode&&!i) {
                size_t blocks=runtime_live,bytes=runtime_bytes;
                runtime_fail_at=runtime_attempts+1;
                CHECK(xir_task_spawn_scoped(witness->executor,view,&witness->group,(XrXirType)256,
                    &request,&witness->members[i])==XR_XIR_CALL_OOM);
                runtime_fail_at=SIZE_MAX;
                XirTaskGroup *group=((XirTask *)(uintptr_t)witness->group.payload)->aggregation;
                CHECK(!witness->members[i].type&&!group->count&&!group->pending&&
                    runtime_live==blocks&&runtime_bytes==bytes);
            }
            CHECK(xir_task_spawn_scoped(witness->executor,view,&witness->group,(XrXirType)256,
                &request,&witness->members[i])==XR_XIR_CALL_READY);
        }
        XirTask *completion=(XirTask *)(uintptr_t)witness->group.payload;
        CHECK(completion->aggregation->count==2&&completion->aggregation->pending==2&&!witness->child_calls);
        if(witness->mode==9) {
            CHECK(xr_xir_call_cleanup_frontier(view,5)==XR_XIR_CALL_READY);
            CHECK(xir_task_scope_begin(witness->executor,view,(XrXirType)257,0,&witness->inner)==XR_XIR_CALL_READY);
            XrXirValue argument={XR_XIR_I64,0,42};const XrXirCallRequest request={.entry=2,.arguments=&argument,.count=1};
            CHECK(xir_task_spawn_scoped(witness->executor,view,&witness->inner,(XrXirType)256,
                &request,&witness->inner_member)==XR_XIR_CALL_READY);
            return (XrXirAction){.kind=XR_XIR_ACTION_RETURN,.value={XR_XIR_I64,0,42}};
        }
        if(witness->mode==3||witness->mode==8)return (XrXirAction){.kind=XR_XIR_ACTION_RETURN,.value={XR_XIR_I64,0,42}};
        if(witness->mode==7)return (XrXirAction){.kind=XR_XIR_ACTION_CONTINUE};
        if(witness->mode==4||witness->mode==6) {
            XrXirAction leave=xr_xir_call_bounds(-9,2);leave.kind=XR_XIR_ACTION_LEAVE;
            leave.flags=XR_XIR_ACTION_LEAVE_PANIC;return leave;
        }
        if(witness->mode==5)return (XrXirAction){.kind=XR_XIR_ACTION_CALL,.callee=4,.flags=XR_XIR_ACTION_PROTECTED};
        CHECK(xir_task_scope_seal(witness->executor,view,&witness->group,witness->mode==1)==XR_XIR_CALL_READY);
        CHECK(xir_task_scope_seal(witness->executor,view,&witness->group,false)==XR_XIR_CALL_BAD_STATE);
        return (XrXirAction){.kind=XR_XIR_ACTION_AWAIT_TASK,.value=witness->group};
    }
    if(witness->mode==7&&!state->body_raised) {
        const XirTask *first=(const XirTask *)(uintptr_t)witness->members[0].payload;
        if(atomic_load_explicit(&first->state,memory_order_acquire)!=XIR_TASK_TERMINAL)
            return (XrXirAction){.kind=XR_XIR_ACTION_CONTINUE};
        CHECK(first->outcome.status==XR_XIR_CALL_BOUNDS);state->body_raised=true;
        XrXirAction leave=xr_xir_call_bounds(-9,2);leave.kind=XR_XIR_ACTION_LEAVE;
        leave.flags=XR_XIR_ACTION_LEAVE_PANIC;return leave;
    }
    if(witness->mode==6||witness->mode==7) {
        CHECK(view->inbox.status==XR_XIR_CALL_BOUNDS&&witness->cleanup_calls==1);
        CHECK(view->inbox.panic.detail.index==(witness->mode==6?-9:-7));
        return (XrXirAction){.kind=XR_XIR_ACTION_RETURN,.value={XR_XIR_I64,0,42}};
    }
    if(witness->mode==4) {
        CHECK(view->inbox.status==XR_XIR_CALL_BOUNDS&&witness->cleanup_calls==1);
        CHECK(((XirTask *)(uintptr_t)witness->group.payload)->aggregation->pending==0);
        return (XrXirAction){.kind=XR_XIR_ACTION_RETURN,.value={XR_XIR_I64,0,42}};
    }
    if(witness->mode==5&&phase==1) {
        CHECK(view->inbox.status==XR_XIR_CALL_BOUNDS&&!witness->cleanup_calls);
        XirTaskGroup *group=((XirTask *)(uintptr_t)witness->group.payload)->aggregation;
        CHECK(!group->sealed&&!group->cancelling);
        CHECK(xir_task_scope_seal(witness->executor,view,&witness->group,false)==XR_XIR_CALL_READY);
        return (XrXirAction){.kind=XR_XIR_ACTION_AWAIT_TASK,.value=witness->group};
    }
    CHECK(view->inbox.status==XR_XIR_CALL_RETURNED&&!view->inbox.value.type);
    return (XrXirAction){.kind=XR_XIR_ACTION_RETURN,.value={XR_XIR_I64,0,42}};
}
static XrXirAction scope_cleanup(XrXirCallView *view) {
    ScopeWitness *witness=(ScopeWitness *)view->environment;++witness->cleanup_calls;
    for(uint32_t i=0;i<2;++i) {
        const XirTask *task=(const XirTask *)(uintptr_t)witness->members[i].payload;
        CHECK(atomic_load_explicit(&task->state,memory_order_acquire)==XIR_TASK_TERMINAL);
    }
    return (XrXirAction){.kind=XR_XIR_ACTION_RETURN};
}
static XrXirAction scope_inner_panic(XrXirCallView *view) {
    (void)view;return xr_xir_call_bounds(-11,2);
}
static XrXirAction scope_inner_cleanup(XrXirCallView *view) {
    ScopeWitness *witness=(ScopeWitness *)view->environment;
    CHECK(((XirTask *)(uintptr_t)witness->inner.payload)->aggregation->pending==0);
    return (XrXirAction){.kind=XR_XIR_ACTION_RETURN};
}
static XrXirAction scope_user_cleanup(XrXirCallView *view) {
    CHECK(view->argument_count==1&&view->arguments[0].type==XR_XIR_I64&&view->arguments[0].payload==42);
    ScopeWitness *witness=(ScopeWitness *)view->environment;witness->user_panic=true;
    return xr_xir_call_bounds(-13,2);
}
static XrXirAction scope_init(XrXirCallView *view) {
    (void)view;return (XrXirAction){.kind=XR_XIR_ACTION_RETURN};
}
static void scope_case(uint32_t mode) {
    NativeFixtureOwner compiler={0};CHECK(native_fixture_owner_new(&compiler)==XR_XIR_OK);
    XrCompileResourceStats baseline=native_fixture_stats(&compiler.context);ScopeWitness witness={.mode=mode};
    const XrXirType parameter=XR_XIR_I64;
    const XrXirCallEntry entries[]={
        {.abi_version=XR_XIR_CALL_ABI_VERSION,.result=XR_XIR_UNIT,.resume=scope_init},
        {.abi_version=XR_XIR_CALL_ABI_VERSION,.result=XR_XIR_I64,.state_bytes=sizeof(ScopeState),
         .flags=XR_XIR_ENTRY_EXIT,.resume=scope_root,.environment=&witness},
        {.abi_version=XR_XIR_CALL_ABI_VERSION,.parameters=&parameter,.parameter_count=1,.result=XR_XIR_I64,
         .state_bytes=sizeof(uint32_t),.flags=XR_XIR_ENTRY_EXIT,.resume=scope_worker,.environment=&witness},
        {.abi_version=XR_XIR_CALL_ABI_VERSION,.result=XR_XIR_UNIT,.cleanup_owner=2,.resume=scope_cleanup,.environment=&witness},
        {.abi_version=XR_XIR_CALL_ABI_VERSION,.result=XR_XIR_UNIT,.resume=scope_inner_panic},
        {.abi_version=XR_XIR_CALL_ABI_VERSION,.result=XR_XIR_UNIT,.cleanup_owner=2,.resume=scope_inner_cleanup,.environment=&witness},
        {.abi_version=XR_XIR_CALL_ABI_VERSION,.parameters=&parameter,.parameter_count=1,
         .result=XR_XIR_UNIT,.cleanup_owner=2,.resume=scope_user_cleanup,.environment=&witness},
        {.abi_version=XR_XIR_CALL_ABI_VERSION,.result=XR_XIR_UNIT,.cleanup_owner=3,.resume=scope_init}};
    XrXirFunctionIdentity identities[8]={{0},{.exported=1},{.exported=1},{.cleanup_owner=2},{.exported=1},
        {.cleanup_owner=2},{.cleanup_owner=2},{.cleanup_owner=3}};
    const XrXirSourceModule source={"scope",5,NULL,0,0,0,XR_XIR_MODULE_FRESH_STORAGE};
    const XrXirDeclarations declarations={&source,1,identities,NULL,0,NULL,0,0,1,NULL};
    const XrXirTypeNode nodes[]={{.kind=XR_XIR_TYPE_TASK,.element=XR_XIR_I64,.result=XR_XIR_NEVER},
        {.kind=XR_XIR_TYPE_TASK,.element=XR_XIR_UNIT,.result=XR_XIR_NEVER}};
    const XrXirTypes types={nodes,2,NULL,NULL};
    XrXirProgramSpec spec={XR_XIR_PROGRAM_ABI_VERSION,{XR_XIR_ARCH_LE64,XR_XIR_VALUE_ABI_VERSION},
        entries,8,&declarations,{0},&types,{0}};
    XrXirArtifact *artifact=NULL;CHECK(native_metadata_fixture(&compiler.context,&spec,&artifact)==XR_XIR_OK);
    XrXirProgramInput *input=NULL;
    CHECK(xir_program_input_take(&compiler.context,&artifact,&input)==XR_XIR_OK&&!artifact);
    XrXirProgram *program=NULL;
    CHECK(xir_program_receive(&compiler.context,&spec,&input,NULL,&program)==XR_XIR_OK&&!input);
    XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(1048576,&domain)==XR_XIR_VALUE_OK);
    const XrXirDomainBudgetControls controls={2097152,2097152,128000000,1048576,1048576};
    CHECK(xr_xir_domain_budget_bind(domain,&controls)==XR_XIR_VALUE_OK);
    XrXirCallBudget budget={.byte_limit=1048576,.requested_limit=2097152,.resume_limit=1000000,.work_domain=domain};
    const XrXirTaskExecutorConfig execution={.program=program,.domain=domain,.task_arena=program->arena,
        .call_limit=1048576,.poll_limit=1000000,.requested_value_limit=2097152,.requested_call_limit=2097152,
        .work_limit=128000000,.depth_limit=64,.budget=&budget};
    CHECK(xr_xir_task_executor_new(&execution,&witness.executor)==XR_XIR_CALL_READY);
    XrXirCallConfig config;CHECK(xr_xir_call_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirCallAccounting accounting={0};config.accounting=&accounting;
    config.entries=program->entries;config.entry_count=program->entry_count;config.byte_limit=1048576;config.poll_limit=1000000;
    config.admission=(XrXirValueAdmission){program->arena,domain,NULL,NULL,1000000,1048576};
    const XrXirCallRequest request={.entry=1};XrXirCall *root=NULL;
    CHECK(xr_xir_call_new_budgeted(&config,&request,&budget,&root)==XR_XIR_CALL_READY);
    CHECK(xr_xir_task_executor_attach_root(witness.executor,root,1)==XR_XIR_CALL_READY);
    XrXirCallResult result={0};unsigned polls=0;bool cancelled=false;
    do {
        CHECK(++polls<128);result=xr_xir_task_executor_poll_root(witness.executor,1);
        if(mode==8&&!cancelled&&witness.group.payload) {
            const XirTaskGroup *group=((XirTask *)(uintptr_t)witness.group.payload)->aggregation;
            if(group->departed&&group->pending&&xr_xir_call_state(root)==XR_XIR_CALL_SUSPENDED) {
                CHECK(xr_xir_task_executor_cancel_root(witness.executor)==XR_XIR_CALL_CANCEL_REQUESTED);
                cancelled=true;
            }
        }
    }while(result.status==XR_XIR_CALL_READY);
    fprintf(stderr,"scope mode%u status%u root%u child%u polls%u\n",mode,result.status,witness.root_calls,witness.child_calls,polls);
    CHECK(result.status==(mode==2||mode==9?XR_XIR_CALL_BOUNDS:mode==8?XR_XIR_CALL_CANCELLED:XR_XIR_CALL_RETURNED));
    if(mode==9)CHECK(result.panic.detail.index==-13);
    CHECK(witness.cleanup_calls==1);
    const uint32_t expected_root[]={4,4,3,3,5,5,6,9,3,6};CHECK(witness.root_calls==expected_root[mode]);
    if(mode==8)CHECK(cancelled);
    XrXirCallResult group={0};
    CHECK(xr_xir_task_copy_outcome(&witness.group,&group)==
        (mode==2||mode==6||mode==7||mode==9?XR_XIR_CALL_BOUNDS:XR_XIR_CALL_RETURNED));
    xr_xir_call_result_drop(&group);CHECK(xr_xir_task_executor_root_idle(witness.executor));
    CHECK(xr_xir_task_executor_free(witness.executor,NULL)==XR_XIR_CALL_READY);
    CHECK(xr_xir_call_free(root)==XR_XIR_CALL_READY);
    xr_xir_value_drop(&witness.group);
    xr_xir_value_drop(&witness.inner);xr_xir_value_drop(&witness.inner_member);
    for(unsigned i=0;i<2;++i)xr_xir_value_drop(&witness.members[i]);
    CHECK(!budget.live_bytes);xr_xir_domain_drop(domain);xr_xir_compile_program_drop(program);
    CHECK(native_fixture_stats(&compiler.context).held_bytes==baseline.held_bytes);native_fixture_owner_free(&compiler);
    CHECK(!runtime_live&&!runtime_bytes);
}
int main(void) {for(uint32_t mode=0;mode<10;++mode)scope_case(mode);return 0;}
