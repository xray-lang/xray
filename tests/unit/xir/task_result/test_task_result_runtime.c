/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_task_result_runtime.c - Actual task leases and one-shot result waits
 */
#include "xir/xxir_program_internal.h"
#include "xir/xxir_task_internal.h"
#include "xir/xxir_task_result_internal.h"
#include "xir/xxir_task_result.h"
#include "xir/xxir_nullable.h"
#include "xir/xxir_enum.h"
#include "xir/xxir_array.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"TaskResult runtime %d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "../xir_runtime_allocations.h"
#include "xir/xxir_task_budget.c"
static XrXirCallStatus observed_completion(XrXirCall *,const XrXirTaskWaitToken *,XrXirCallResult *);
#define xr_xir_call_complete_task_wait observed_completion
#include "xir/xxir_task.c"
#undef xr_xir_call_complete_task_wait
extern const XrXirProgramSpec source_artifact_program;

enum Scenario { SNAPSHOT, CANCELLED_RESULT, ORDINARY_AFTER_RESULT, DUPLICATE_STAGE,
    WRONG_ACTION, WRONG_TASK, IDENTITY_RETRY, FOREIGN_TASK, CONSTRUCTOR_FAULT,
    SELF_CANCEL, WAIT_FAILURE, WRAP_LIMIT, ROOT_YIELD };
static enum Scenario scenario;
static XrXirValue subject,other,captured;
static XrXirType task_type;
static uint32_t worker_entry,poll_site,result_site,probe_entry,panic_entry;
static unsigned phase,completions;
static bool active,inject;
static size_t fault_relative,wrap_allocations;
static XrXirAction action(XrXirActionKind kind,int64_t value) {
    XrXirAction result={0};result.kind=kind;
    if(kind==XR_XIR_ACTION_RETURN || kind==XR_XIR_ACTION_FAULT)result.value=(XrXirValue){XR_XIR_I64,0,value};
    return result;
}
static XrXirAction wait_action(const XrXirValue *task) {
    XrXirAction result={0};result.kind=XR_XIR_ACTION_AWAIT_TASK;result.value=*task;return result;
}
static void pending_poll(XrXirCallView *view) {
    XrXirValue value={0};const XirTask *task=(const XirTask *)(uintptr_t)subject.payload;
    XirTaskState state=atomic_load_explicit(&task->state,memory_order_acquire);
    CHECK(state==XIR_TASK_PENDING);uint64_t ticket=task->identity;
    CHECK(xr_xir_instance_task_poll_at(view,poll_site,&subject,&value)==XR_XIR_CALL_READY);
    bool some=true;const XrXirValue *payload=NULL;
    CHECK(xr_xir_nullable_view(&value,&some,&payload) && !some && !payload);
    CHECK(atomic_load_explicit(&task->state,memory_order_acquire)==state && task->identity==ticket);
    xr_xir_value_drop(&value);XrXirCallResult untouched={0};
    CHECK(xr_xir_task_copy_outcome(&subject,&untouched)==XR_XIR_CALL_BAD_STATE && xr_xir_call_result_empty(&untouched));
    if(scenario==SNAPSHOT) {
        XrXirValueAdmission *admission=xr_xir_call_admission(view);uint64_t work=admission->work;
        admission->work=0;
        CHECK(xr_xir_instance_task_poll_at(view,poll_site,&subject,&value)==XR_XIR_CALL_LIMIT && !value.type && !value.payload);
        admission->work=work;
        CHECK(atomic_load_explicit(&task->state,memory_order_acquire)==state && task->identity==ticket);
    }
}
static XrXirAction control(XrXirCallView *view) {
    if(view->phase==XR_XIR_CALL_EXIT) {
        CHECK(!view->activation->top->task_result && !view->activation->top->task_result_subject);
        return action(XR_XIR_ACTION_EXIT_DONE,0);
    }
    if(scenario==FOREIGN_TASK) {
        XrXirValue value={0};CHECK(xr_xir_instance_task_poll_at(view,poll_site,&subject,&value)==XR_XIR_CALL_BAD_ARGUMENT);
        CHECK(!value.type && !value.reserved && !value.payload);
        CHECK(xr_xir_instance_task_await_result_at(view,result_site,&subject)==XR_XIR_CALL_BAD_ARGUMENT);
        CHECK(!view->activation->top->task_result);return action(XR_XIR_ACTION_RETURN,7);
    }
    if(!phase++) {
        CHECK(!subject.type);XrXirCallStatus spawned=xr_xir_task_go(view,task_type,worker_entry,NULL,0,&subject);
        if(spawned!=XR_XIR_CALL_READY)fprintf(stderr,"go status%u task%u worker%u current%u scenario%u\n",
            spawned,(uint32_t)task_type,worker_entry,xr_xir_call_current_entry(view->activation),(unsigned)scenario);
        CHECK(spawned==XR_XIR_CALL_READY);
        pending_poll(view);
        XrXirCallView copy=*view;XrXirValue empty={0};uint64_t epoch=view->activation->top->epoch;
        CHECK(xr_xir_instance_task_poll_at(&copy,poll_site,&subject,&empty)!=XR_XIR_CALL_READY && !empty.type);
        CHECK(xr_xir_instance_task_await_result_at(&copy,result_site,&subject)!=XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_task_await_result_at(view,UINT32_MAX,&subject)!=XR_XIR_CALL_READY);
        CHECK(!view->activation->top->task_result && view->activation->top->epoch==epoch);
        if(scenario==CANCELLED_RESULT)CHECK(xr_xir_task_cancel(view,&subject)==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_task_await_result_at(view,result_site,&subject)==XR_XIR_CALL_READY);
        CHECK(view->activation->top->task_result && view->activation->top->task_result_subject==(const void *)(uintptr_t)subject.payload);
        if(scenario==SELF_CANCEL)CHECK(xr_xir_call_request_cancel(view->activation)==XR_XIR_CALL_CANCEL_REQUESTED);
        if(scenario==DUPLICATE_STAGE) {
            CHECK(xr_xir_instance_task_await_result_at(view,result_site,&subject)==XR_XIR_CALL_BAD_STATE);
            CHECK(!view->activation->top->task_result && !view->activation->top->task_result_subject);
        }
        if(scenario==WRONG_ACTION)return action(XR_XIR_ACTION_CONTINUE,0);
        if(scenario==WRONG_TASK) {
            CHECK(xr_xir_task_go(view,task_type,worker_entry,NULL,0,&other)==XR_XIR_CALL_READY);return wait_action(&other);
        }
        return wait_action(&subject);
    }
    CHECK(!view->activation->top->task_result && !view->activation->top->task_result_subject);
    CHECK(view->inbox.status==XR_XIR_CALL_RETURNED);
    if(scenario==DUPLICATE_STAGE || (scenario==ORDINARY_AFTER_RESULT && phase==3)) {
        CHECK(view->inbox.value.type==XR_XIR_I64 && view->inbox.value.payload==42);return action(XR_XIR_ACTION_RETURN,7);
    }
    uint32_t variant=UINT32_MAX;CHECK(xr_xir_enum_variant(&view->inbox.value,&variant)==XR_XIR_VALUE_OK);
    CHECK(variant==(uint32_t)(scenario==CANCELLED_RESULT?XR_XIR_TASK_RESULT_CANCELLED:XR_XIR_TASK_RESULT_SUCCESS));
    if(!captured.type)CHECK(xr_xir_value_copy(&view->inbox.value,&captured)==XR_XIR_VALUE_OK);
    if(scenario==ORDINARY_AFTER_RESULT && phase==2)return wait_action(&subject);
    XrXirValue first={0},second={0};
    CHECK(xr_xir_instance_task_poll_at(view,poll_site,&subject,&first)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_task_poll_at(view,poll_site,&subject,&second)==XR_XIR_CALL_READY);
    xr_xir_value_drop(&first);CHECK(xr_xir_value_valid(&second));xr_xir_value_drop(&second);
    return action(XR_XIR_ACTION_RETURN,7);
}
static XrXirCallStatus observed_completion(XrXirCall *call,const XrXirTaskWaitToken *token,XrXirCallResult *outcome) {
    if(!active || !call->top->task_result)return xr_xir_call_complete_task_wait(call,token,outcome);
    ++completions;uint64_t epoch=call->top->epoch;XrXirType mode=call->top->task_result;
    CHECK(!xir_call_bound_yield_pending(call,&token->binding));
    if(scenario==WAIT_FAILURE) {
        CHECK(xr_xir_call_fail_task_wait(call,token,XR_XIR_CALL_OOM)==XR_XIR_CALL_READY);
        CHECK(!call->top->task_result && !call->top->task_result_subject && call->abort_reason==XR_XIR_CALL_OOM);
        CHECK(outcome->status==XR_XIR_CALL_RETURNED && outcome->value.type==XR_XIR_I64 && outcome->value.payload==42);
        return XR_XIR_CALL_READY;
    }
    if(scenario==IDENTITY_RETRY) {
        XrXirTaskWaitToken wrong=*token;++wrong.wake;
        CHECK(xr_xir_call_complete_task_wait(call,&wrong,outcome)==XR_XIR_CALL_BAD_STATE);
        CHECK(call->top->epoch==epoch && call->top->task_result==mode);
        CHECK(outcome->status==XR_XIR_CALL_RETURNED && outcome->value.type==XR_XIR_I64);
        XrXirCallResult other_outcome=*outcome;++other_outcome.value.payload;
        CHECK(xr_xir_call_complete_task_wait(call,token,&other_outcome)==XR_XIR_CALL_BAD_STATE);
        CHECK(other_outcome.value.payload==outcome->value.payload+1 && call->top->task_result==mode);
    }
    bool selected=scenario==CONSTRUCTOR_FAULT && xr_xir_call_panic_status(outcome->status);
    size_t before=runtime_attempts,bytes=runtime_bytes,blocks=runtime_live;
    if(selected && inject)runtime_fail_at=before+fault_relative;
    uint64_t work=call->config.admission.work;
    if(scenario==WRAP_LIMIT)call->config.admission.work=0;
    XrXirCallStatus result=xr_xir_call_complete_task_wait(call,token,outcome);runtime_fail_at=SIZE_MAX;
    if(scenario==WRAP_LIMIT) {
        call->config.admission.work=work;
        CHECK(call->abort_reason==XR_XIR_CALL_LIMIT && runtime_bytes==bytes && runtime_live==blocks);
    }
    CHECK(result==XR_XIR_CALL_READY && call->top->epoch==epoch && !call->top->task_result && !call->top->task_result_subject);
    if(selected) {
        if(inject)CHECK(runtime_attempts>before+fault_relative && call->abort_reason==XR_XIR_CALL_OOM &&
            runtime_bytes==bytes && runtime_live==blocks);
        else {wrap_allocations=runtime_attempts-before;CHECK(wrap_allocations>=2);}
    }
    return result;
}
static uint32_t named(const XrXirModule *module,const char *name) {
    for(uint32_t f=0;f<module->function_count;++f)
        if(module->functions[f].name_length==strlen(name) && !memcmp(module->functions[f].name,name,strlen(name)))return f;
    CHECK(0);return UINT32_MAX;
}
static XrXirOutputStatus discard_output(void *context,XrXirOutputStream stream,const char *bytes,size_t length) {
    (void)context;(void)stream;(void)bytes;(void)length;return XR_XIR_OUTPUT_OK;
}
static XrXirCallStatus execute(XrXirInstance *instance,uint32_t entry) {
    CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY);
    XrXirCallStatus status=xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status;
    if(status==XR_XIR_CALL_RETURNED) {
        XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==status);xr_xir_value_drop(&value);
    }
    return status;
}
static XrXirValue export_value(XrXirInstance *instance,uint32_t entry) {
    CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status==XR_XIR_CALL_RETURNED);
    XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);return value;
}
static void raw_family_controls(void) {
    XrCompileResourceLimits limits={0};limits.memory_mode=XR_COMPILE_MEMORY_ORDINARY;
    XrXirCompileContext context={0};context.limits=xr_xir_compile_default_limits();
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats baseline={0},final={0};CHECK(xr_compile_resources_stats(context.resources,&baseline)==XR_COMPILE_RESOURCE_OK);
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_program_seal(&context,&source_artifact_program,&program)==XR_XIR_OK);
    const XrXirModule *module=xr_xir_compile_artifact_module(program->permissions->input->artifact);
    XrXirOutputSink sink={XR_XIR_CALL_ABI_VERSION,0,discard_output,NULL,SIZE_MAX};
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,xr_xir_output_render,&sink};
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    CHECK(execute(instance,source_artifact_program.declarations->entry_function)==XR_XIR_CALL_RETURNED);
    XrXirValue error_result=export_value(instance,named(module,"errorResult"));
    XrXirValue unit_result=export_value(instance,named(module,"unitResult"));
    XrXirValue panic_result=export_value(instance,named(module,"panicResult"));
    XrXirValue optional=export_value(instance,named(module,"unsafePanicInfo"));
    XrXirValue array_result=export_value(instance,named(module,"resultArray"));
    XrXirType error_type=(XrXirType)error_result.type,unit_type=(XrXirType)unit_result.type,array_type=(XrXirType)array_result.type;
    const XrXirTypeArena *arena=program->arena;XrXirDomain *domain=object_pointer(&error_result)->domain;
    CHECK(xr_xir_domain_retain(domain));uint64_t held=0;
    XrXirValueAdmission admission={.arena=arena,.domain=domain,.work=UINT64_MAX,.scratch_bytes=UINT64_MAX};
    XrXirValue error_a={0},safe_info={0},unsafe_info={0};
    CHECK(xr_xir_enum_get(&error_result,XR_XIR_TASK_RESULT_ERROR,0,&admission,&error_a)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_enum_get(&panic_result,XR_XIR_TASK_RESULT_PANIC,0,&admission,&safe_info)==XR_XIR_VALUE_OK);
    bool some=false;const XrXirValue *borrowed=NULL;
    CHECK(xr_xir_nullable_view(&optional,&some,&borrowed) && some && borrowed && borrowed->type==XR_XIR_PANIC_INFO);
    CHECK(xr_xir_value_copy(borrowed,&unsafe_info)==XR_XIR_VALUE_OK);
    xr_xir_value_drop(&error_result);xr_xir_value_drop(&unit_result);xr_xir_value_drop(&panic_result);
    xr_xir_value_drop(&optional);xr_xir_value_drop(&array_result);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);xr_xir_compile_program_drop(program);
    held=xr_xir_domain_stats(domain).live_bytes;
    CHECK(xr_xir_value_valid(&error_a) && xr_xir_value_valid(&safe_info) && xr_xir_value_valid(&unsafe_info));
    XirPanicInfo *safe=(XirPanicInfo *)object_pointer(&safe_info);
    XirPanicInfo *unsafe=(XirPanicInfo *)object_pointer(&unsafe_info);
    const XrXirValue *error_b=&safe->panic.error;CHECK(error_b->type==XR_XIR_ERROR);
    XrXirValue saved_b=*error_b,refused={0},accepted={0};
    CHECK(xr_xir_enum_new(error_type,XR_XIR_TASK_RESULT_ERROR,&error_a,1,&admission,&accepted)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_value_admit(&accepted,error_type,&admission)==XR_XIR_VALUE_OK);
    uint64_t before=xr_xir_domain_stats(domain).live_bytes;
    CHECK(xr_xir_enum_new(error_type,XR_XIR_TASK_RESULT_ERROR,error_b,1,&admission,&refused)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(!refused.type && !refused.reserved && !refused.payload && xr_xir_domain_stats(domain).live_bytes==before);
    CHECK(error_b->type==saved_b.type && error_b->reserved==saved_b.reserved && error_b->payload==saved_b.payload);
    CHECK(xr_xir_enum_new(unit_type,XR_XIR_TASK_RESULT_ERROR,&error_a,1,&admission,&refused)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(!refused.type && !refused.payload && xr_xir_domain_stats(domain).live_bytes==before);
    XirNominalValue *record=(XirNominalValue *)object_pointer(&accepted);XrXirValue saved={0};
    CHECK(record->variant==XR_XIR_TASK_RESULT_ERROR && record->count==1);
    CHECK(xr_xir_value_move(&record->fields[0],&saved)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_value_copy(error_b,&record->fields[0])==XR_XIR_VALUE_OK);
    CHECK(xr_xir_value_admit(&accepted,error_type,&admission)==XR_XIR_VALUE_BAD_ARGUMENT);
    xr_xir_value_drop(&record->fields[0]);CHECK(xr_xir_value_move(&saved,&record->fields[0])==XR_XIR_VALUE_OK);
    CHECK(xr_xir_value_admit(&accepted,error_type,&admission)==XR_XIR_VALUE_OK);xr_xir_value_drop(&accepted);
    CHECK(xr_xir_value_admit(&unsafe_info,XR_XIR_PANIC_INFO,&admission)==XR_XIR_VALUE_OK);
    before=xr_xir_domain_stats(domain).live_bytes;
    CHECK(xr_xir_enum_new(unit_type,XR_XIR_TASK_RESULT_PANIC,&unsafe_info,1,&admission,&refused)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(!refused.type && !refused.payload && xr_xir_domain_stats(domain).live_bytes==before && xr_xir_value_valid(&unsafe_info));
    XrXirValue fresh={0},boxed={0},packed={0};
    CHECK(xr_xir_panic_info_new(domain,&safe->panic,&fresh)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_enum_new(unit_type,XR_XIR_TASK_RESULT_PANIC,&fresh,1,&admission,&boxed)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_array_new(array_type,&boxed,1,&admission,&packed)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_value_admit(&boxed,unit_type,&admission)==XR_XIR_VALUE_OK && xr_xir_value_admit(&packed,array_type,&admission)==XR_XIR_VALUE_OK);
    XirPanicInfo *mutable=(XirPanicInfo *)object_pointer(&fresh);
    CHECK(xr_xir_value_move(&mutable->panic.replaced_error,&saved)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_value_copy(&unsafe->panic.replaced_error,&mutable->panic.replaced_error)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_value_admit(&fresh,XR_XIR_PANIC_INFO,&admission)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_value_admit(&boxed,unit_type,&admission)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_value_admit(&packed,array_type,&admission)==XR_XIR_VALUE_BAD_ARGUMENT);
    xr_xir_value_drop(&mutable->panic.replaced_error);
    CHECK(xr_xir_value_move(&saved,&mutable->panic.replaced_error)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_value_admit(&boxed,unit_type,&admission)==XR_XIR_VALUE_OK && xr_xir_value_admit(&packed,array_type,&admission)==XR_XIR_VALUE_OK);
    xr_xir_value_drop(&packed);xr_xir_value_drop(&boxed);xr_xir_value_drop(&fresh);
    CHECK(xr_xir_domain_stats(domain).live_bytes==held);
    xr_xir_value_drop(&error_a);xr_xir_value_drop(&safe_info);xr_xir_value_drop(&unsafe_info);
    CHECK(xr_xir_domain_stats(domain).live_bytes==sizeof(*domain));xr_xir_domain_drop(domain);
    CHECK(xr_compile_resources_stats(context.resources,&final)==XR_COMPILE_RESOURCE_OK && final.held_bytes==baseline.held_bytes);
    xr_compile_resources_release(context.resources);CHECK(!runtime_live && !runtime_bytes);
    puts("TaskResult raw APIs: exact E/Never, strong boxed/packed Panic graphs, preserved local owners and recovery PASS");
}
static XrXirCallStatus root_yield_tokens(XrXirInstance *instance,uint32_t entry,XrXirCallResult *owned) {
    CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY);
    XrXirInstanceResult first=xr_xir_instance_poll_bounded(instance,64);
    CHECK(first.outcome.status==XR_XIR_CALL_SUSPENDED && first.epoch && first.outcome.wake);
    CHECK(instance->executor->ready_head && instance->executor->external==&instance->executor->root);
    XrXirExecutorBinding binding={instance->executor,&instance->executor->root,
        instance->executor->generation,instance->executor->root.ticket};
    CHECK(xir_call_bound_yield_pending(instance->call,&binding));
    XrXirExecutorBinding forged=binding;++forged.generation;
    CHECK(!xir_call_bound_yield_pending(instance->call,&forged));
    forged=binding;++forged.ticket;CHECK(!xir_call_bound_yield_pending(instance->call,&forged));
    forged=binding;forged.activation=&forged;CHECK(!xir_call_bound_yield_pending(instance->call,&forged));
    XrXirWaitRequest request={0};
    CHECK(xr_xir_instance_wait_request(instance,first.epoch,first.outcome.wake,&request)==XR_XIR_CALL_READY && request.kind==XR_XIR_WAIT_YIELD);
    XrXirInstanceResult again=xr_xir_instance_poll_bounded(instance,1);
    CHECK(again.outcome.status==XR_XIR_CALL_SUSPENDED && again.epoch==first.epoch && again.outcome.wake==first.outcome.wake);
    CHECK(instance->executor->ready_head && instance->executor->external==&instance->executor->root);
    CHECK(xr_xir_instance_resume(instance,first.epoch+1,first.outcome.wake)==XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_instance_resume(instance,first.epoch,first.outcome.wake+1)==XR_XIR_CALL_BAD_STATE);
    XrXirWaitRequest untouched={0};
    CHECK(xr_xir_instance_wait_request(instance,first.epoch,first.outcome.wake+1,&untouched)==XR_XIR_CALL_BAD_STATE && !untouched.kind);
    CHECK(xr_xir_instance_resume(instance,first.epoch,first.outcome.wake)==XR_XIR_CALL_READY);
    CHECK(!xir_call_bound_yield_pending(instance->call,&binding));
    CHECK(xr_xir_instance_resume(instance,first.epoch,first.outcome.wake)==XR_XIR_CALL_BAD_STATE);
    unsigned steps=0;XrXirInstanceResult terminal={0};
    do {CHECK(++steps<10000);terminal=xr_xir_instance_poll_bounded(instance,64);}
    while(terminal.outcome.status==XR_XIR_CALL_READY);
    CHECK(terminal.outcome.status==XR_XIR_CALL_CLEANUP_FAILED && !terminal.outcome.value.type);
    CHECK(xr_xir_call_result_copy(&terminal.outcome,owned)==XR_XIR_VALUE_OK);
    return terminal.outcome.status;
}
static void run_case(enum Scenario selected,bool fault,size_t relative) {
    scenario=selected;inject=fault;fault_relative=relative;phase=completions=0;active=false;
    XrCompileResourceLimits limits={0};limits.memory_mode=XR_COMPILE_MEMORY_ORDINARY;
    XrXirCompileContext context={0};context.limits=xr_xir_compile_default_limits();
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats baseline={0},final={0};CHECK(xr_compile_resources_stats(context.resources,&baseline)==XR_COMPILE_RESOURCE_OK);
    XrXirArtifact *metadata=NULL;CHECK(xr_xir_compile_checked_read(&context,source_artifact_program.packet.bytes,source_artifact_program.packet.length,&metadata,NULL)==XR_XIR_OK);
    const XrXirModule *module=xr_xir_compile_artifact_module(metadata);probe_entry=named(module,"runtimeProbe");panic_entry=named(module,"panicResult");
    uint32_t fatal_entry=named(module,"fatalResult");
    xr_xir_compile_artifact_free(metadata);
    XrXirProgramSpec spec=source_artifact_program;XrXirCallEntry *entries=xr_malloc((size_t)spec.entry_count*sizeof(*entries));CHECK(entries);
    memcpy(entries,spec.entries,(size_t)spec.entry_count*sizeof(*entries));spec.entries=entries;
    bool original_panic=selected==CONSTRUCTOR_FAULT || selected==WRAP_LIMIT;
    if(!original_panic)entries[probe_entry].resume=control;
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_program_seal(&context,&spec,&program)==XR_XIR_OK);xr_free(entries);
    module=xr_xir_compile_artifact_module(program->permissions->input->artifact);
    const XrXirFunction *body=&module->functions[probe_entry];poll_site=result_site=UINT32_MAX;task_type=XR_XIR_UNIT;
    for(uint32_t i=0;i<body->instruction_count;++i) {
        const XrXirInstruction *op=&body->instructions[i];
        if(op->op==XR_XIR_GO){task_type=op->type;worker_entry=(uint32_t)op->immediate;}
        if(op->op==XR_XIR_TASK_POLL)poll_site=i;
        if(op->op==XR_XIR_TASK_AWAIT_RESULT)result_site=i;
    }
    CHECK(task_type!=XR_XIR_UNIT && poll_site!=UINT32_MAX && result_site!=UINT32_MAX);
    XrXirOutputSink sink={XR_XIR_CALL_ABI_VERSION,0,discard_output,NULL,SIZE_MAX};
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,xr_xir_output_render,&sink};
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    CHECK(execute(instance,spec.declarations->entry_function)==XR_XIR_CALL_RETURNED);active=true;
    XrXirCallResult terminal_owner={0};
    XrXirCallStatus status=selected==ROOT_YIELD?root_yield_tokens(instance,fatal_entry,&terminal_owner):
        execute(instance,original_panic?panic_entry:probe_entry);
    XrXirCallStatus expected=selected==WRONG_ACTION || selected==WRONG_TASK?XR_XIR_CALL_BAD_STATE:
        (selected==CONSTRUCTOR_FAULT && fault) || selected==WAIT_FAILURE?XR_XIR_CALL_OOM:
        selected==WRAP_LIMIT?XR_XIR_CALL_LIMIT:selected==SELF_CANCEL?XR_XIR_CALL_CANCELLED:
        selected==ROOT_YIELD?XR_XIR_CALL_CLEANUP_FAILED:XR_XIR_CALL_RETURNED;
    CHECK(status==expected && (completions || selected==WRONG_ACTION || selected==WRONG_TASK || selected==DUPLICATE_STAGE || selected==SELF_CANCEL || selected==ROOT_YIELD));active=false;
    XrXirCallStatus closed=xr_xir_instance_free(instance);
    CHECK(closed==(expected==XR_XIR_CALL_RETURNED || expected==XR_XIR_CALL_CANCELLED?XR_XIR_CALL_READY:expected));
    if(selected==SNAPSHOT) {
        scenario=FOREIGN_TASK;instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
        CHECK(execute(instance,spec.declarations->entry_function)==XR_XIR_CALL_RETURNED);
        CHECK(execute(instance,probe_entry)==XR_XIR_CALL_RETURNED && xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
        CHECK(xr_xir_value_valid(&subject));
    }
    if(captured.type){CHECK(xr_xir_value_valid(&captured));xr_xir_value_drop(&captured);}
    if(subject.type)xr_xir_value_drop(&subject);if(other.type)xr_xir_value_drop(&other);
    xr_xir_compile_program_drop(program);
    if(selected==ROOT_YIELD) {
        CHECK(xr_xir_call_result_valid(&terminal_owner) && terminal_owner.panic.detail.code==420 && terminal_owner.prior_status==XR_XIR_CALL_CANCELLED);
        xr_xir_call_result_drop(&terminal_owner);
    }
    CHECK(xr_compile_resources_stats(context.resources,&final)==XR_COMPILE_RESOURCE_OK && final.held_bytes==baseline.held_bytes);
    xr_compile_resources_release(context.resources);if(runtime_live || runtime_bytes)runtime_report_residuals(stderr);CHECK(!runtime_live && !runtime_bytes);
}
int main(void) {
    run_case(SNAPSHOT,false,0);run_case(CANCELLED_RESULT,false,0);run_case(ORDINARY_AFTER_RESULT,false,0);
    run_case(DUPLICATE_STAGE,false,0);run_case(WRONG_ACTION,false,0);run_case(WRONG_TASK,false,0);run_case(IDENTITY_RETRY,false,0);
    run_case(CONSTRUCTOR_FAULT,false,0);size_t allocations=wrap_allocations;
    run_case(CONSTRUCTOR_FAULT,true,0);run_case(CONSTRUCTOR_FAULT,true,allocations-1);
    run_case(SELF_CANCEL,false,0);run_case(WAIT_FAILURE,false,0);run_case(WRAP_LIMIT,false,0);
    run_case(ROOT_YIELD,false,0);
    raw_family_controls();
    puts("TaskResult runtime: pending snapshot, one-shot authenticated waits, identity retry, own cancellation and OOM/LIMIT rollback PASS");return 0;
}
