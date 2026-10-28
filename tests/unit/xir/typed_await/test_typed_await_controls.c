/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_typed_await_controls.c - Trusted native ingress and physical task rollback
 *
 * KEY CONCEPT:
 *   Instrumented callbacks test runtime admission separately from generated code.
 */
#include "xir/xxir_program_internal.h"
#include "xir/xxir_task_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if (!(c)) {fprintf(stderr,"typed await controls %d: %s\n",__LINE__,#c);exit(1);}} while (0)
#include "../xir_runtime_allocations.h"
#include "xir/xxir_task_budget.c"
#include "xir/xxir_task.c"
extern const XrXirProgramSpec source_artifact_program;
enum Scenario { CANCEL_CHILD, CANCEL_SELF, LATE_CANCEL, FOREIGN_CANCEL, SPAWN_OOM };
static enum Scenario scenario;
static XrXirValue subject;
static XrXirType task_type;
static uint32_t worker_entry;
static size_t spawn_allocations,fault_relative;
static unsigned self_requests,self_exits;
static XrXirAction action(XrXirActionKind kind,int64_t value) {
    return (XrXirAction){.kind=kind,.value={kind==XR_XIR_ACTION_RETURN || kind==XR_XIR_ACTION_FAULT?XR_XIR_I64:XR_XIR_UNIT,0,value}};
}
static XrXirAction initialize(XrXirCallView *view) {
    return view->phase==XR_XIR_CALL_EXIT?action(XR_XIR_ACTION_EXIT_DONE,0):action(XR_XIR_ACTION_RETURN,0);
}
static XrXirAction initialize_unit(XrXirCallView *view) {
    return view->phase==XR_XIR_CALL_EXIT?action(XR_XIR_ACTION_EXIT_DONE,0):(XrXirAction){.kind=XR_XIR_ACTION_RETURN};
}
static void allocation_free_cancel(XrXirCallView *view,XrXirCallStatus expected) {
    size_t attempts=runtime_attempts,live=runtime_live,bytes=runtime_bytes;
    XrXirCallStatus actual=xr_xir_task_cancel(view,&subject);
    if (actual!=expected) fprintf(stderr,"cancel scenario%u actual%u expected%u\n",(unsigned)scenario,actual,expected);
    CHECK(actual==expected);
    CHECK(runtime_attempts==attempts && runtime_live==live && runtime_bytes==bytes);
}
static XrXirAction self_worker(XrXirCallView *view) {
    if (view->phase==XR_XIR_CALL_EXIT) {++self_exits;return action(XR_XIR_ACTION_EXIT_DONE,0);}
    CHECK(scenario==CANCEL_SELF && !self_requests++);
    allocation_free_cancel(view,XR_XIR_CALL_READY);
    CHECK(xr_xir_call_execution_status(view)==XR_XIR_CALL_CANCELLED);
    allocation_free_cancel(view,XR_XIR_CALL_CANCELLED);
    XrXirCallResult empty={0};CHECK(xr_xir_task_copy_outcome(&subject,&empty)==XR_XIR_CALL_BAD_STATE && xr_xir_call_result_empty(&empty));
    CHECK(!self_exits);return action(XR_XIR_ACTION_SUSPEND,0);
}
static XrXirAction control(XrXirCallView *view) {
    if (view->phase==XR_XIR_CALL_EXIT) return action(XR_XIR_ACTION_EXIT_DONE,0);
    if (scenario==FOREIGN_CANCEL) {allocation_free_cancel(view,XR_XIR_CALL_BAD_ARGUMENT);return action(XR_XIR_ACTION_RETURN,7);}
    if (scenario==LATE_CANCEL) {
        allocation_free_cancel(view,XR_XIR_CALL_READY);
        XirTask *task=(XirTask *)(uintptr_t)subject.payload;uint64_t generation=task->generation;
        task->generation=0;allocation_free_cancel(view,XR_XIR_CALL_BAD_ARGUMENT);task->generation=generation;
        uint64_t identity=task->identity;task->identity=UINT64_MAX;
        allocation_free_cancel(view,XR_XIR_CALL_BAD_ARGUMENT);task->identity=identity;
        CHECK(xr_xir_task_cancel(view,NULL)==XR_XIR_CALL_BAD_ARGUMENT);
        XrXirCallView copy=*view;CHECK(xr_xir_task_cancel(&copy,&subject)!=XR_XIR_CALL_READY);
        allocation_free_cancel(view,XR_XIR_CALL_READY);return action(XR_XIR_ACTION_RETURN,7);
    }
    CHECK(!subject.type && !subject.payload);
    size_t before=runtime_attempts,live=runtime_live,bytes=runtime_bytes;
    if (scenario==SPAWN_OOM) runtime_fail_at=before+fault_relative;
    XrXirCallStatus status=xr_xir_task_go(view,task_type,worker_entry,NULL,0,&subject);
    runtime_fail_at=SIZE_MAX;
    if (scenario==SPAWN_OOM) {
        CHECK(status==XR_XIR_CALL_OOM && !subject.type && !subject.reserved && !subject.payload);
        CHECK(runtime_live==live && runtime_bytes==bytes);return action(XR_XIR_ACTION_FAULT,XR_XIR_CALL_OOM);
    }
    if (status!=XR_XIR_CALL_READY) fprintf(stderr,"actual go refused status%u Task%u worker%u current%u\n",
        status,(uint32_t)task_type,worker_entry,xr_xir_call_current_entry(view->activation));
    CHECK(status==XR_XIR_CALL_READY && subject.type==(uint32_t)task_type);
    spawn_allocations=runtime_attempts-before;CHECK(spawn_allocations>1);
    if (scenario==CANCEL_CHILD) {
        XrXirValueAdmission *admission=xr_xir_call_admission(view);uint64_t work=admission->work;
        admission->work=0;allocation_free_cancel(view,XR_XIR_CALL_LIMIT);admission->work=work;
        allocation_free_cancel(view,XR_XIR_CALL_READY);allocation_free_cancel(view,XR_XIR_CALL_READY);
        XrXirCallResult empty={0};CHECK(xr_xir_task_copy_outcome(&subject,&empty)==XR_XIR_CALL_BAD_STATE && xr_xir_call_result_empty(&empty));
    }
    return action(XR_XIR_ACTION_RETURN,7);
}
static uint32_t find(const XrXirModule *module,const char *name) {
    for (uint32_t f=0;f<module->function_count;++f)
        if (module->functions[f].name_length==strlen(name) && !memcmp(module->functions[f].name,name,strlen(name))) return f;
    CHECK(false);return UINT32_MAX;
}
static XrXirCallStatus execute(XrXirInstance *instance,uint32_t entry) {
    CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY);
    XrXirInstanceResult result=xr_xir_instance_poll_bounded(instance,UINT64_MAX);
    if (result.outcome.status==XR_XIR_CALL_RETURNED) {
        XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED && value.type==XR_XIR_I64 && value.payload==7);
        xr_xir_value_drop(&value);
    }
    return result.outcome.status;
}
static void run_case(enum Scenario selected,size_t failure) {
    scenario=selected;fault_relative=failure;self_requests=self_exits=0;
    XrCompileResourceLimits limits={0};limits.memory_mode=XR_COMPILE_MEMORY_ORDINARY;
    XrXirCompileContext context={0};context.limits=xr_xir_compile_default_limits();
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats baseline={0},final={0};CHECK(xr_compile_resources_stats(context.resources,&baseline)==XR_COMPILE_RESOURCE_OK);
    XrXirArtifact *checked=NULL;
    CHECK(xr_xir_compile_checked_read(&context,source_artifact_program.packet.bytes,source_artifact_program.packet.length,&checked,NULL)==XR_XIR_OK);
    const XrXirModule *module=xr_xir_compile_artifact_module(checked);
    uint32_t probe=find(module,"childCancellation");worker_entry=find(module,"spinning");task_type=XR_XIR_UNIT;
    CHECK(module->function_count==source_artifact_program.entry_count);
    const XrXirTypes *execution_types=source_artifact_program.types;
    for (uint32_t t=0;t<execution_types->count;++t) {
        const XrXirTypeNode *node=&execution_types->nodes[t];
        if (node->kind==XR_XIR_TYPE_TASK && node->element==XR_XIR_I64 && node->result==XR_XIR_NEVER)
            task_type=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+t);
    }
    CHECK(task_type!=XR_XIR_UNIT);xr_xir_compile_artifact_free(checked);
    XrXirProgramSpec spec=source_artifact_program;XrXirCallEntry entries[64];CHECK(spec.entry_count<=64);
    memcpy(entries,spec.entries,(size_t)spec.entry_count*sizeof(*entries));spec.entries=entries;
    CHECK(!spec.declarations->slot_count);
    for (uint32_t m=0;m<spec.declarations->module_count;++m)
        entries[spec.declarations->modules[m].initializer].resume=initialize_unit;
    entries[spec.declarations->entry_function].resume=initialize;entries[probe].resume=control;
    if (selected==CANCEL_SELF) entries[worker_entry].resume=self_worker;
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_program_seal(&context,&spec,&program)==XR_XIR_OK);
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    XrXirCallStatus actual=execute(instance,probe),expected=selected==SPAWN_OOM?XR_XIR_CALL_OOM:XR_XIR_CALL_RETURNED;
    if (actual!=expected) fprintf(stderr,"scenario%u actual%u expected%u subject%u allocations%zu self%u exits%u\n",
        (unsigned)selected,actual,expected,subject.type,spawn_allocations,self_requests,self_exits);
    CHECK(actual==expected);
    if (selected!=SPAWN_OOM) {
        XrXirCallResult result={0};CHECK(xr_xir_task_copy_outcome(&subject,&result)==XR_XIR_CALL_CANCELLED && !result.value.type);
        xr_xir_call_result_drop(&result);
        if (selected==CANCEL_SELF) CHECK(self_requests==1 && self_exits==1);
        scenario=LATE_CANCEL;CHECK(execute(instance,probe)==XR_XIR_CALL_RETURNED);
    }
    XrXirCallStatus closed=xr_xir_instance_free(instance);
    CHECK(closed==(selected==SPAWN_OOM?XR_XIR_CALL_OOM:XR_XIR_CALL_READY));
    if (selected!=SPAWN_OOM) {
        scenario=FOREIGN_CANCEL;instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
        CHECK(execute(instance,probe)==XR_XIR_CALL_RETURNED);
        CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
        XrXirCallResult result={0};CHECK(xr_xir_task_copy_outcome(&subject,&result)==XR_XIR_CALL_CANCELLED);
        xr_xir_call_result_drop(&result);xr_xir_value_drop(&subject);
    }
    xr_xir_compile_program_drop(program);
    CHECK(xr_compile_resources_stats(context.resources,&final)==XR_COMPILE_RESOURCE_OK && final.held_bytes==baseline.held_bytes);
    xr_compile_resources_release(context.resources);
    if (runtime_live || runtime_bytes) runtime_report_residuals(stderr);
    CHECK(!runtime_live && !runtime_bytes);
}
int main(void) {
    run_case(CANCEL_CHILD,0);size_t allocations=spawn_allocations;
    run_case(CANCEL_SELF,0);run_case(SPAWN_OOM,0);run_case(SPAWN_OOM,allocations-1);
    puts("trusted native controls: authenticated self/foreign/epoch/late requests, allocation-free cancel and physical OOM rollback PASS");return 0;
}
