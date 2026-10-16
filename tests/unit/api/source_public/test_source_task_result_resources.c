/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_task_result_resources.c - Actual Task result cuts and pending authority
 */
#include "xray_source_entry.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "xir/xir_runtime_allocations.h"
#include "base/xcompile_resources.c"
#include "xir/xxir_vm.c"
#include "xir/xxir_task_budget.c"
#include "xir/xxir_task.c"
#include "execution/xr_xir_host_execution.c"
#include "api/xsource_api.c"
#include "source_public_task_result_fixture.h"
typedef struct TaskResultOwners {
    XrSourceCompiler *compiler;
    XrSourceProgram *program;
    XrSourceCall *call;
    XrSourceValue *task;
    XrSourceResult *result,*copy;
    uint64_t runtime_work;
} TaskResultOwners;
static XrSourceWriteStatus task_discard(void *context,XrSourceOutputStream stream,const char *bytes,size_t length) {
    (void)context;CHECK(stream==XR_SOURCE_STDOUT || stream==XR_SOURCE_STDERR);CHECK(bytes || !length);return XR_SOURCE_WRITE_OK;
}
static void task_result_close(TaskResultOwners *owner) {
    CHECK(xray_source_call_drop(&owner->call,NULL)==XR_SOURCE_OK);
    xray_source_value_free(owner->task);xray_source_result_free(owner->result);xray_source_result_free(owner->copy);
    xray_source_program_free(owner->program);xray_source_compiler_free(owner->compiler);
    *owner=(TaskResultOwners){0};CHECK(!runtime_live && !runtime_bytes);
}
static XrSourceStatus task_result_prepare(TaskResultOwners *owner,const XrCompileResourceLimits *limits) {
    XrSourceCompilerOptions options;CHECK(xray_source_compiler_options_init(&options,sizeof(options))==XR_SOURCE_OK);
    options.stdlib_path=XR_PUBLIC_STDLIB;options.allocated_bytes=limits->allocated_bytes;
    options.live_bytes=limits->live_bytes;options.work=limits->work;
    XrSourceStatus status=xray_source_compiler_new(&options,&owner->compiler);if(status!=XR_SOURCE_OK)return status;
    const XrSourceText text={NULL,"task-result-resources",task_result_source,sizeof(task_result_source)-1};
    return xray_source_compile_text(owner->compiler,&text,&owner->program,NULL);
}
static XrSourceInvocation task_result_invocation(const char *name) {
    XrSourceInvocation request;CHECK(xray_source_invocation_init(&request,sizeof(request))==XR_SOURCE_OK);
    request.name=name;request.name_length=strlen(name);return request;
}
static XrSourceStatus task_result_ready(TaskResultOwners *owner,const char *name) {
    XrSourceCallOptions options;CHECK(xray_source_call_options_init(&options,sizeof(options))==XR_SOURCE_OK);options.write=task_discard;
    if(owner->runtime_work)options.work_limit=owner->runtime_work;
    XrSourceInvocation request=task_result_invocation(name);
    XrSourceStatus status=xray_source_invoke(owner->program,&request,&options,&owner->result,NULL);
    if(status==XR_SOURCE_OK)status=xray_source_result_value(owner->result,&owner->task);
    xray_source_result_free(owner->result);owner->result=NULL;
    xray_source_program_free(owner->program);owner->program=NULL;
    xray_source_compiler_free(owner->compiler);owner->compiler=NULL;return status;
}
static XrSourceStatus task_result_drive(TaskResultOwners *owner) {
    XrSourceStatus status=xray_source_value_task_result(owner->task,&owner->result);
    if(status==XR_SOURCE_OK)status=xray_source_value_task_result(owner->task,&owner->copy);
    return status;
}
static void task_result_faults(void) {
    static const char *names[]={"numberTask","textTask","arrayTask","unitTask","nestedTask","errorTask","panicTask"};
    const XrCompileResourceLimits limits={UINT64_C(64)<<20,UINT64_C(8)<<20,UINT64_C(128000000)};
    for(size_t mode=0;mode<7;++mode) {
        size_t sites=0;
        for(size_t point=0;!point || point<=sites;++point) {
            runtime_fail_at=SIZE_MAX;TaskResultOwners owner={0};
            CHECK(task_result_prepare(&owner,&limits)==XR_SOURCE_OK && task_result_ready(&owner,names[mode])==XR_SOURCE_OK);
            runtime_attempts=0;runtime_fail_at=point?point-1:SIZE_MAX;
            XrSourceStatus status=task_result_drive(&owner);
            if(!point){CHECK(status==XR_SOURCE_OK && owner.copy);sites=runtime_attempts;CHECK(sites);}
            else {
                CHECK(status==XR_SOURCE_OUT_OF_MEMORY && !owner.copy && runtime_attempts>runtime_fail_at);
                runtime_fail_at=SIZE_MAX;
                if(owner.result){xray_source_result_free(owner.result);owner.result=NULL;}
                CHECK(task_result_drive(&owner)==XR_SOURCE_OK && owner.copy);
            }
            runtime_fail_at=SIZE_MAX;task_result_close(&owner);
        }
        printf("public Task result mode=%s actual sites=%zu swept=%zu; retry same Task, physical=0/0\n",names[mode],sites,sites);
    }
}
static void task_result_pending(void) {
    const XrCompileResourceLimits limits={UINT64_C(64)<<20,UINT64_C(8)<<20,UINT64_C(128000000)};
    for(unsigned mode=0;mode<4;++mode) {
        TaskResultOwners owner={0};CHECK(task_result_prepare(&owner,&limits)==XR_SOURCE_OK);
        XrSourceCallOptions options;CHECK(xray_source_call_options_init(&options,sizeof(options))==XR_SOURCE_OK);
        options.write=task_discard;options.work_limit=mode==2?10000:1000000;
        if(mode==1)options.poll_limit=2000;
        XrSourceInvocation request=task_result_invocation(mode==3?"allocatingTask":"pendingTask");
        CHECK(xray_source_invoke_begin(owner.program,&request,&options,&owner.call)==XR_SOURCE_OK);
        XrXirTaskExecutor *executor=owner.call->host->instance->executor;
        for(unsigned step=0;!executor->active && step<4096;++step) {
            XrSourceEvent event;CHECK(xray_source_call_poll(owner.call,1,&event)==XR_SOURCE_OK && event.state==XR_SOURCE_RUN_READY);
        }
        CHECK(executor->active);XirTask *task=executor->active;
        XrXirValue actual=task_core_task_value(task);
        CHECK(api_value_wrap(owner.program->resources,&actual,&owner.task)==XR_SOURCE_OK);
        XrCompileResourceStats before={0},after={0};
        CHECK(xr_compile_resources_stats(owner.task->resources,&before)==XR_COMPILE_RESOURCE_OK);
        size_t attempts=runtime_attempts;uint64_t epoch=executor->root_epoch,identity=executor->next_identity;
        XirTaskActivation *head=executor->ready_head;
        XrXirDomainBudgetStats domain_before=xr_xir_domain_budget_stats(task->object.domain);
        CHECK(xray_source_value_task_result(owner.task,&owner.result)==XR_SOURCE_BAD_STATE && !owner.result);
        XrXirDomainBudgetStats domain_after=xr_xir_domain_budget_stats(task->object.domain);
        CHECK(xr_compile_resources_stats(owner.task->resources,&after)==XR_COMPILE_RESOURCE_OK);
        CHECK(!memcmp(&before,&after,sizeof(before)) && runtime_attempts==attempts && executor->root_epoch==epoch &&
            executor->next_identity==identity && executor->ready_head==head && task->executor==executor);
        CHECK(!memcmp(&domain_before,&domain_after,sizeof(domain_before)));
        if(!mode) {
            CHECK(xray_source_call_cancel(owner.call)==XR_SOURCE_OK);
            CHECK(xray_source_call_drop(&owner.call,NULL)==XR_SOURCE_OK && !owner.call);
        }
        size_t fault=runtime_attempts;
        if(mode==3)runtime_fail_at=fault;
        XrSourceEvent event={0};
        for(unsigned step=0;mode && step<20000;++step) {
            CHECK(xray_source_call_poll(owner.call,64,&event)==XR_SOURCE_OK);
            if(event.state!=XR_SOURCE_RUN_READY)break;
        }
        runtime_fail_at=SIZE_MAX;
        printf("pending Task control mode=%u root state=%u\n",mode,(unsigned)event.state);fflush(stdout);
        if(mode)CHECK(event.state==(mode==3?XR_SOURCE_RUN_OUT_OF_MEMORY:XR_SOURCE_RUN_LIMIT));
        if(mode==3)CHECK(runtime_attempts>fault && task->outcome.status==XR_XIR_CALL_OOM);
        CHECK(xray_source_call_drop(&owner.call,NULL)==XR_SOURCE_OK && !owner.call);
        xray_source_program_free(owner.program);owner.program=NULL;
        xray_source_compiler_free(owner.compiler);owner.compiler=NULL;
        if(mode==2)CHECK(xray_source_value_task_result(owner.task,&owner.result)==XR_SOURCE_LIMIT && !owner.result);
        else {
            CHECK(xray_source_value_task_result(owner.task,&owner.result)==XR_SOURCE_OK && owner.result);
            CHECK(xray_source_result_event(owner.result,&event)==XR_SOURCE_OK &&
                event.state==(!mode?XR_SOURCE_RUN_CANCELLED:mode==3?XR_SOURCE_RUN_OUT_OF_MEMORY:XR_SOURCE_RUN_LIMIT));
        }
        task_result_close(&owner);
    }
    puts("actual Source child Task pending: no allocation/work/queue/epoch changes; cancel/LIMIT/OOM outcomes differ from copy LIMIT, physical=0/0");
}
static void task_result_budgets(void) {
    const XrCompileResourceLimits generous={UINT64_C(64)<<20,UINT64_C(8)<<20,UINT64_C(128000000)};
    TaskResultOwners owner={0};XrCompileResourceStats required={0};
    CHECK(task_result_prepare(&owner,&generous)==XR_SOURCE_OK && task_result_ready(&owner,"arrayTask")==XR_SOURCE_OK);
    CHECK(task_result_drive(&owner)==XR_SOURCE_OK);
    CHECK(xr_compile_resources_stats(owner.task->resources,&required)==XR_COMPILE_RESOURCE_OK);
    XirTask *task=(XirTask *)(uintptr_t)owner.task->value.payload;
    uint64_t runtime_work=xr_xir_domain_budget_stats(task->object.domain).work;
    task_result_close(&owner);
    for(unsigned axis=0;axis<3;++axis)for(int delta=-1;delta<=1;++delta) {
        XrCompileResourceLimits limits={required.allocated_bytes,required.peak_bytes,required.work};
        uint64_t *bound=axis==0?&limits.allocated_bytes:axis==1?&limits.live_bytes:&limits.work;
        CHECK(*bound>1 && *bound<UINT64_MAX);*bound=(uint64_t)((int64_t)*bound+delta);
        XrSourceStatus status=task_result_prepare(&owner,&limits);
        if(status==XR_SOURCE_OK)status=task_result_ready(&owner,"arrayTask");
        if(status==XR_SOURCE_OK)status=task_result_drive(&owner);
        if(delta<0)CHECK(status==XR_SOURCE_BUDGET && !owner.copy);
        else CHECK(status==XR_SOURCE_OK && owner.copy);
        task_result_close(&owner);
    }
    CHECK(runtime_work>1 && runtime_work<UINT64_MAX);
    for(int delta=-1;delta<=1;++delta) {
        owner.runtime_work=(uint64_t)((int64_t)runtime_work+delta);
        CHECK(task_result_prepare(&owner,&generous)==XR_SOURCE_OK && task_result_ready(&owner,"arrayTask")==XR_SOURCE_OK);
        XrSourceStatus status=task_result_drive(&owner);
        task=(XirTask *)(uintptr_t)owner.task->value.payload;
        if(delta<0) {
            CHECK(status==XR_SOURCE_LIMIT && owner.result && !owner.copy);
            size_t attempts=runtime_attempts;XrXirDomainBudgetStats spent=xr_xir_domain_budget_stats(task->object.domain);
            CHECK(xray_source_value_task_result(owner.task,&owner.copy)==XR_SOURCE_LIMIT && !owner.copy && runtime_attempts==attempts);
            CHECK(xr_xir_domain_budget_stats(task->object.domain).work==spent.work);
        } else CHECK(status==XR_SOURCE_OK && owner.copy);
        task_result_close(&owner);
    }
    printf("public Task result compiler axes allocated=%llu peak=%llu work=%llu; runtime work=%llu exact/minus-one/plus-one, no refund, physical=0/0\n",
        (unsigned long long)required.allocated_bytes,(unsigned long long)required.peak_bytes,
        (unsigned long long)required.work,(unsigned long long)runtime_work);
}
int main(void) {task_result_faults();task_result_pending();task_result_budgets();return 0;}
