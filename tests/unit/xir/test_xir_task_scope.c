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
    XrXirValue group,members[2];
    uint32_t mode,root_calls,child_calls;
} ScopeWitness;
static XrXirAction scope_worker(XrXirCallView *view) {
    ScopeWitness *witness=(ScopeWitness *)view->environment;++witness->child_calls;
    uint32_t *phase=view->state;
    if(view->arguments[0].payload<0)return xr_xir_call_bounds(-7,2);
    if(witness->mode==2&&!(*phase)++)return (XrXirAction){.kind=XR_XIR_ACTION_SUSPEND};
    if((*phase)++<2)return (XrXirAction){.kind=XR_XIR_ACTION_CONTINUE};
    return (XrXirAction){.kind=XR_XIR_ACTION_RETURN,.value={XR_XIR_I64,0,view->arguments[0].payload}};
}
static XrXirAction scope_root(XrXirCallView *view) {
    ScopeWitness *witness=(ScopeWitness *)view->environment;++witness->root_calls;
    uint32_t *phase=view->state;
    if(!(*phase)++) {
        CHECK(xir_task_scope_begin(witness->executor,view,(XrXirType)257,1,&witness->group)==XR_XIR_CALL_BAD_ARGUMENT);
        CHECK(xir_task_scope_begin(witness->executor,view,(XrXirType)257,0,&witness->group)==XR_XIR_CALL_READY);
        XrXirCallView forged=*view;XrXirValue refused={0};
        CHECK(xir_task_scope_begin(witness->executor,&forged,(XrXirType)257,0,&refused)!=XR_XIR_CALL_READY&&!refused.type);
        for(uint32_t i=0;i<2;++i) {
            XrXirValue argument={XR_XIR_I64,0,witness->mode==2&&!i?-1:20+(int64_t)i*2};
            const XrXirCallRequest request={.entry=2,.arguments=&argument,.count=1};
            CHECK(xir_task_spawn_scoped(witness->executor,view,&witness->group,(XrXirType)256,
                &request,&witness->members[i])==XR_XIR_CALL_READY);
        }
        XirTask *completion=(XirTask *)(uintptr_t)witness->group.payload;
        CHECK(completion->aggregation->count==2&&completion->aggregation->pending==2&&!witness->child_calls);
        CHECK(xir_task_scope_seal(witness->executor,view,&witness->group,witness->mode==1)==XR_XIR_CALL_READY);
        CHECK(xir_task_scope_seal(witness->executor,view,&witness->group,false)==XR_XIR_CALL_BAD_STATE);
        return (XrXirAction){.kind=XR_XIR_ACTION_AWAIT_TASK,.value=witness->group};
    }
    CHECK(view->inbox.status==XR_XIR_CALL_RETURNED&&!view->inbox.value.type);
    return (XrXirAction){.kind=XR_XIR_ACTION_RETURN,.value={XR_XIR_I64,0,42}};
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
        {.abi_version=XR_XIR_CALL_ABI_VERSION,.result=XR_XIR_I64,.state_bytes=sizeof(uint32_t),.resume=scope_root,.environment=&witness},
        {.abi_version=XR_XIR_CALL_ABI_VERSION,.parameters=&parameter,.parameter_count=1,.result=XR_XIR_I64,
         .state_bytes=sizeof(uint32_t),.resume=scope_worker,.environment=&witness}};
    XrXirFunctionIdentity identities[3]={{0},{.exported=1},{.exported=1}};
    const XrXirSourceModule source={"scope",5,NULL,0,0,0,XR_XIR_MODULE_FRESH_STORAGE};
    const XrXirDeclarations declarations={&source,1,identities,NULL,0,NULL,0,0,1,NULL};
    const XrXirTypeNode nodes[]={{.kind=XR_XIR_TYPE_TASK,.element=XR_XIR_I64,.result=XR_XIR_NEVER},
        {.kind=XR_XIR_TYPE_TASK,.element=XR_XIR_UNIT,.result=XR_XIR_NEVER}};
    const XrXirTypes types={nodes,2,NULL,NULL};
    XrXirProgramSpec spec={XR_XIR_PROGRAM_ABI_VERSION,{XR_XIR_ARCH_LE64,XR_XIR_VALUE_ABI_VERSION},
        entries,3,&declarations,{0},&types,{0}};
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
    XrXirCallResult result={0};unsigned polls=0;
    do {CHECK(++polls<128);result=xr_xir_task_executor_poll_root(witness.executor,1);}while(result.status==XR_XIR_CALL_READY);
    fprintf(stderr,"scope mode%u status%u root%u child%u polls%u\n",mode,result.status,witness.root_calls,witness.child_calls,polls);
    CHECK(result.status==(mode==2?XR_XIR_CALL_BOUNDS:XR_XIR_CALL_RETURNED));
    CHECK(witness.root_calls==(mode==2?1u:2u));
    XrXirCallResult group={0};CHECK(xr_xir_task_copy_outcome(&witness.group,&group)==result.status);
    xr_xir_call_result_drop(&group);CHECK(xr_xir_task_executor_root_idle(witness.executor));
    CHECK(xr_xir_call_free(root)==XR_XIR_CALL_READY);
    CHECK(xr_xir_task_executor_free(witness.executor,NULL)==XR_XIR_CALL_READY);
    xr_xir_value_drop(&witness.group);
    for(unsigned i=0;i<2;++i)xr_xir_value_drop(&witness.members[i]);
    CHECK(!budget.live_bytes);xr_xir_domain_drop(domain);xr_xir_compile_program_drop(program);
    CHECK(native_fixture_stats(&compiler.context).held_bytes==baseline.held_bytes);native_fixture_owner_free(&compiler);
    CHECK(!runtime_live&&!runtime_bytes);
}
int main(void) {for(uint32_t mode=0;mode<3;++mode)scope_case(mode);return 0;}
