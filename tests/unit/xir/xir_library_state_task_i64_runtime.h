/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_library_state_task_i64_runtime.h - Independent Instance state and retained values
 */
#ifndef XIR_LIBRARY_STATE_TASK_I64_RUNTIME_H
#define XIR_LIBRARY_STATE_TASK_I64_RUNTIME_H
#include "xir/xxir.h"
XR_FUNC void xr_test_library_state_task_i64_run(const XrXirCompileContext *context,XrXirArtifact *owned);
#ifdef XR_LIBRARY_STATE_TASK_I64_RUNTIME_IMPLEMENTATION
#include "xir/xxir_output.h"
#include "xir/xxir_task.h"
#include "xir/xxir_error.h"
#include "xir/xxir_enum.h"
enum { LIBRARY_STATE_TASK_I64_EXPORTS=13 };
typedef struct LibraryStateTaskI64Output { char bytes[64];size_t length; } LibraryStateTaskI64Output;
static XrXirOutputStatus library_state_task_i64_write(void *context,XrXirOutputStream stream,const char *bytes,size_t length) {
    LibraryStateTaskI64Output *output=context;
    CHECK(stream==XR_XIR_STDOUT && length<=sizeof(output->bytes)-output->length);
    memcpy(output->bytes+output->length,bytes,length);output->length+=length;return XR_XIR_OUTPUT_OK;
}
static void library_state_task_i64_held(const XrXirValue *value) {
    const char *bytes=NULL;size_t length=0;
    CHECK(value->type==XR_XIR_STRING && xr_xir_string_view(value,&bytes,&length));
    CHECK(length==5 && !memcmp(bytes,"seed!",5));
}
static XrXirValue *library_state_task_i64_pending;
/* Observe the actual backend's awaited Source GO owner while its callback is
 * active. This retains a real value without changing the returned action. */
static void library_state_task_i64_capture(const XrXirAction *action) {
    if (library_state_task_i64_pending && !library_state_task_i64_pending->type &&
        action->kind==XR_XIR_ACTION_AWAIT_TASK)
        CHECK(xr_xir_value_copy(&action->value,library_state_task_i64_pending)==XR_XIR_VALUE_OK);
}
#if CONSUMER_KIND!=2
typedef struct LibraryStateTaskI64Observed {
    XrXirVmBinding vm;
    XrXirCallEntry actual;
} LibraryStateTaskI64Observed;
static LibraryStateTaskI64Observed library_state_task_i64_observed[128];
static XrXirAction library_state_task_i64_resume(XrXirCallView *view) {
    LibraryStateTaskI64Observed *binding=(LibraryStateTaskI64Observed *)view->environment;
    XrXirAction action=binding->actual.resume(view);library_state_task_i64_capture(&action);return action;
}
static void library_state_task_i64_release(XrXirCallView *view,XrXirCallStatus reason) {
    LibraryStateTaskI64Observed *binding=(LibraryStateTaskI64Observed *)view->environment;
    if (binding->actual.release) binding->actual.release(view,reason);
}
static void library_state_task_i64_observe(XrXirProgramSpec *spec,XrXirCallEntry entries[128]) {
    CHECK(spec->entry_count<=128);
    for (uint32_t f=0;f<spec->entry_count;++f) {
        LibraryStateTaskI64Observed *binding=&library_state_task_i64_observed[f];
        binding->actual=spec->entries[f];
#if CONSUMER_KIND==0 || CONSUMER_KIND==3
        CHECK(binding->actual.environment);binding->vm=*(const XrXirVmBinding *)binding->actual.environment;
#else
        CHECK(!binding->actual.environment);
#endif
        entries[f]=binding->actual;entries[f].environment=binding;
        entries[f].resume=library_state_task_i64_resume;entries[f].release=library_state_task_i64_release;
    }
    spec->entries=entries;
}
#endif
static void library_state_task_i64_task(const XrXirValue *value,int64_t expected) {
    CHECK(xr_xir_task_element(xr_xir_compile_type_arena_types(xr_xir_value_arena(value)),(XrXirType)value->type)==XR_XIR_I64);
    for (uint32_t repeat=0;repeat<2;++repeat) {
        XrXirCallResult outcome={0};CHECK(xr_xir_task_copy_outcome(value,&outcome)==XR_XIR_CALL_RETURNED);
        CHECK(outcome.value.type==XR_XIR_I64 && outcome.value.payload==expected);xr_xir_call_result_drop(&outcome);
    }
}
static void library_state_task_i64_error(const XrXirValue *task) {
    for (uint32_t repeat=0;repeat<2;++repeat) {
        XrXirCallResult outcome={0};CHECK(xr_xir_task_copy_outcome(task,&outcome)==XR_XIR_CALL_THROWN);
        XrXirValue concrete={0};CHECK(xr_xir_error_borrow(&outcome.value,&concrete));
        XrXirEnumBorrow borrowed={0};CHECK(xr_xir_enum_borrow(&concrete,&borrowed)==XR_XIR_VALUE_OK);
        CHECK(borrowed.field_count==1 && borrowed.fields && borrowed.member.length==6 &&
            !memcmp(borrowed.member.bytes,"Failed",6));
        const char *bytes=NULL;size_t length=0;CHECK(xr_xir_string_view(&borrowed.fields[0],&bytes,&length));
        CHECK(length==5 && !memcmp(bytes,"owned",5));xr_xir_call_result_drop(&outcome);
    }
}
static void library_state_task_i64_cancelled(const XrXirValue *task) {
    for (uint32_t repeat=0;repeat<2;++repeat) {
        XrXirCallResult outcome={0};CHECK(xr_xir_task_copy_outcome(task,&outcome)==XR_XIR_CALL_CANCELLED);
        CHECK(!outcome.value.type && !outcome.value.payload);xr_xir_call_result_drop(&outcome);
    }
}
#if CONSUMER_KIND==0 || CONSUMER_KIND==3
static void library_state_task_i64_ids(const XrXirModule *module,uint32_t ids[LIBRARY_STATE_TASK_I64_EXPORTS]) {
    static const char *const library_state_task_i64_names[LIBRARY_STATE_TASK_I64_EXPORTS]={"run","ownValue","held","pureWorker","snapshot","taskHeld","constAwait","constReader","rootHandleWorker","errorHeld","awaitError","cancelWait","defaultEmpty"};
    for (uint32_t e=0;e<LIBRARY_STATE_TASK_I64_EXPORTS;++e) {
        ids[e]=UINT32_MAX;
        for (uint32_t f=0;f<module->function_count;++f) {
            const XrXirFunction *fn=&module->functions[f];
            const XrXirFunctionIdentity *identity=&module->declarations->functions[f];
            if (identity->module!=module->declarations->root_module || fn->name_length!=strlen(library_state_task_i64_names[e]) ||
                memcmp(fn->name,library_state_task_i64_names[e],fn->name_length)) continue;
            CHECK(identity->exported && ids[e]==UINT32_MAX);ids[e]=f;
        }
        CHECK(ids[e]!=UINT32_MAX);
    }
}
#endif
static bool library_state_task_i64_call(XrXirInstance *instance,uint32_t entry,int64_t expected,XrXirValue *held) {
    XrXirCallStatus status=xr_xir_instance_start(instance,entry,NULL,0);
    if (status==XR_XIR_CALL_READY) status=xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status;
    if (status==XR_XIR_CALL_OOM) return false;
    CHECK(status==XR_XIR_CALL_RETURNED);
    XrXirValue value={0};status=xr_xir_instance_take_result(instance,&value);
    if (status==XR_XIR_CALL_OOM) return false;
    CHECK(status==XR_XIR_CALL_RETURNED);
    if (held) {
        if (value.type==XR_XIR_STRING) library_state_task_i64_held(&value);
        else if (expected>=0) library_state_task_i64_task(&value,expected);
        else library_state_task_i64_error(&value);
        *held=value;
    }
    else {CHECK(value.type==XR_XIR_I64 && value.payload==expected);xr_xir_value_drop(&value);}
    return true;
}
static bool library_state_task_i64_await_error(XrXirInstance *instance,uint32_t entry) {
    XrXirCallStatus status=xr_xir_instance_start(instance,entry,NULL,0);
    if (status==XR_XIR_CALL_READY) status=xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status;
    if (status==XR_XIR_CALL_OOM) return false;
    CHECK(status==XR_XIR_CALL_THROWN && xr_xir_instance_state(instance)==XR_XIR_INSTANCE_READY);return true;
}
static bool library_state_task_i64_cancel(XrXirInstance *instance,uint32_t entry,XrXirValue *held) {
    XrXirCallStatus status=xr_xir_instance_start(instance,entry,NULL,0);
    if (status==XR_XIR_CALL_OOM) return false;
    CHECK(status==XR_XIR_CALL_READY);library_state_task_i64_pending=held;
    for (uint32_t step=0;step<256 && !held->type;++step) {
        status=xr_xir_instance_poll_bounded(instance,1).outcome.status;
        if (status==XR_XIR_CALL_OOM) {library_state_task_i64_pending=NULL;return false;}
        CHECK(status==XR_XIR_CALL_READY);
    }
    library_state_task_i64_pending=NULL;CHECK(held->type);
    XrXirCallResult outcome={0};CHECK(xr_xir_task_copy_outcome(held,&outcome)==XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_call_result_empty(&outcome));
    CHECK(xr_xir_instance_cancel_current(instance)==XR_XIR_CALL_CANCEL_REQUESTED);
    CHECK(xr_xir_instance_stop(instance)==XR_XIR_CALL_READY);
    library_state_task_i64_cancelled(held);return true;
}
static bool library_state_task_i64_pair(XrXirProgram *program,const uint32_t ids[LIBRARY_STATE_TASK_I64_EXPORTS],XrXirValue held[8]) {
    XrXirInstance *instances[2]={0};LibraryStateTaskI64Output outputs[2]={0};XrXirOutputSink sinks[2]={0};bool ok=false;
    for (uint32_t i=0;i<2;++i) {
        XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
        sinks[i]=(XrXirOutputSink){XR_XIR_CALL_ABI_VERSION,0,library_state_task_i64_write,&outputs[i],64};
        config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,xr_xir_output_render,&sinks[i]};
        XrXirCallStatus status=xr_xir_instance_new(program,&config,&instances[i]);
        if (status==XR_XIR_CALL_OOM) goto done;
        CHECK(status==XR_XIR_CALL_READY);
    }
    for (uint32_t i=0;i<2;++i) {
        CHECK(!outputs[i].length);
        if (!library_state_task_i64_call(instances[i],ids[1],100,NULL) ||
            !library_state_task_i64_call(instances[i],ids[3],41,NULL) ||
            !library_state_task_i64_call(instances[i],ids[4],1,NULL) ||
            !library_state_task_i64_call(instances[i],ids[12],1,NULL) ||
            !library_state_task_i64_call(instances[i],ids[6],40,NULL) ||
            !library_state_task_i64_call(instances[i],ids[6],40,NULL) ||
            !library_state_task_i64_call(instances[i],ids[7],41,NULL)) goto done;
    }
    for (uint32_t step=0;step<2;++step) for (uint32_t i=0;i<2;++i) {
        size_t peer=outputs[1-i].length;
        if (!library_state_task_i64_call(instances[i],ids[0],step+1,NULL)) goto done;
        CHECK(outputs[1-i].length==peer);
        const char *expected=step ? "1\nseed!\n2\nseed!!\n" : "1\nseed!\n";
        size_t length=step ? 17 : 8;
        CHECK(outputs[i].length==length && !memcmp(outputs[i].bytes,expected,length));
        if (!step && !library_state_task_i64_call(instances[i],ids[2],0,&held[i])) goto done;
        if (!step && !library_state_task_i64_call(instances[i],ids[5],1,&held[i+2])) goto done;
        if (!library_state_task_i64_call(instances[i],ids[8],41,NULL)) goto done;
        library_state_task_i64_held(&held[i]);library_state_task_i64_task(&held[i+2],1);
    }
    for (uint32_t i=0;i<2;++i) if (!library_state_task_i64_call(instances[i],ids[1],102,NULL) ||
        !library_state_task_i64_call(instances[i],ids[4],3,NULL)) goto done;
    CHECK(held[2].payload!=held[3].payload);
    for (uint32_t i=0;i<2;++i) {
        if (!library_state_task_i64_call(instances[i],ids[9],-1,&held[i+4]) ||
            !library_state_task_i64_await_error(instances[i],ids[10]) ||
            !library_state_task_i64_cancel(instances[i],ids[11],&held[i+6])) goto done;
    }
    CHECK(held[4].payload!=held[5].payload && held[6].payload!=held[7].payload);ok=true;
done:
    for (uint32_t i=0;i<2;++i) if (instances[i]) {
        XrXirCallStatus freed=xr_xir_instance_free(instances[i]);
        CHECK(freed==XR_XIR_CALL_READY || (!ok && freed==XR_XIR_CALL_OOM));
    }
    return ok;
}
typedef struct LibraryStateTaskI64Costs {uint64_t value,call,work;} LibraryStateTaskI64Costs;
/* Measure the real Instance domain, including disposal, for this fixed program.
 * No cap is raised; exact and minus-one runs retain the original defaults. */
static XrXirCallStatus library_state_task_i64_budget_operation(XrXirProgram *program,
    const uint32_t ids[LIBRARY_STATE_TASK_I64_EXPORTS],LibraryStateTaskI64Costs limits,LibraryStateTaskI64Costs *costs) {
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    LibraryStateTaskI64Output output={0};XrXirOutputSink sink={XR_XIR_CALL_ABI_VERSION,0,library_state_task_i64_write,&output,64};
    config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,xr_xir_output_render,&sink};
    if (limits.value) config.requested_value_limit=limits.value;
    if (limits.call) config.requested_call_limit=limits.call;
    if (limits.work) config.work_limit=limits.work;
    XrXirInstance *instance=NULL;XrXirDomain *domain=NULL;XrXirValue value={0};
    XrXirCallStatus status=xr_xir_instance_new(program,&config,&instance);
    if (status!=XR_XIR_CALL_READY) goto done;
    domain=instance->domain;CHECK(xr_xir_domain_retain(domain));
    const uint32_t sequence[7]={1,12,6,0,8,0,5};const int64_t expected[7]={100,1,40,1,41,2,2};
    for (uint32_t s=0;s<7;++s) {
        status=xr_xir_instance_start(instance,ids[sequence[s]],NULL,0);
        if (status==XR_XIR_CALL_READY) status=xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status;
        if (status!=XR_XIR_CALL_RETURNED) goto done;
        status=xr_xir_instance_take_result(instance,&value);if (status!=XR_XIR_CALL_RETURNED) goto done;
        if (s==6) {
            XrXirCallResult outcome={0};status=xr_xir_task_copy_outcome(&value,&outcome);
            if (status==XR_XIR_CALL_RETURNED) CHECK(outcome.value.type==XR_XIR_I64 && outcome.value.payload==2);
            xr_xir_call_result_drop(&outcome);if (status!=XR_XIR_CALL_RETURNED) goto done;
        } else CHECK(value.type==XR_XIR_I64 && value.payload==expected[s]);
        xr_xir_value_drop(&value);
    }
    CHECK(output.length==17 && !memcmp(output.bytes,"1\nseed!\n2\nseed!!\n",17));
done:
    xr_xir_value_drop(&value);
    if (instance) {
        XrXirCallStatus freed=xr_xir_instance_free(instance);
        if (freed!=XR_XIR_CALL_READY) {if (status==XR_XIR_CALL_RETURNED) status=freed;else CHECK(freed==status);}
    }
    if (domain && costs) {
        XrXirDomainBudgetStats measured=xr_xir_domain_budget_stats(domain);
        *costs=(LibraryStateTaskI64Costs){measured.requested_bytes,measured.requested_call_bytes,measured.work};
    }
    xr_xir_domain_drop(domain);return status;
}
static void library_state_task_i64_axes(XrXirProgram *program,const uint32_t ids[LIBRARY_STATE_TASK_I64_EXPORTS]) {
    size_t live=runtime_live,bytes=runtime_bytes;LibraryStateTaskI64Costs required={0};
    CHECK(library_state_task_i64_budget_operation(program,ids,(LibraryStateTaskI64Costs){0},&required)==XR_XIR_CALL_RETURNED);
    CHECK(required.value && required.call && required.work && runtime_live==live && runtime_bytes==bytes);
    for (uint32_t axis=0;axis<3;++axis) for (uint32_t less=0;less<2;++less) {
        LibraryStateTaskI64Costs limits={0},actual={0};
        if (!axis) limits.value=required.value-less;
        else if (axis==1) limits.call=required.call-less;
        else limits.work=required.work-less;
        XrXirCallStatus status=library_state_task_i64_budget_operation(program,ids,limits,&actual);
        CHECK(status==(less ? XR_XIR_CALL_LIMIT : XR_XIR_CALL_RETURNED));
        CHECK((!limits.value || actual.value<=limits.value) && (!limits.call || actual.call<=limits.call) &&
            (!limits.work || actual.work<=limits.work) && runtime_live==live && runtime_bytes==bytes);
    }
    printf("Task private state actual Instance value/call/work exact-minus1 %llu/%llu/%llu physicalbaseline\n",
        (unsigned long long)required.value,(unsigned long long)required.call,(unsigned long long)required.work);
}
static XrXirStatus library_state_task_i64_seal_operation(const XrXirCompileContext *context,void *opaque) {
    XrXirProgram *program=NULL;XrXirStatus status=xr_xir_compile_program_seal(context,opaque,&program);
    CHECK(status==XR_XIR_OK ? program!=NULL : program==NULL);xr_xir_compile_program_drop(program);return status;
}
static void library_state_task_i64_finish(const XrXirCompileContext *context,XrXirProgramSpec *spec,XrXirArtifact *lowered,
    const uint32_t ids[LIBRARY_STATE_TASK_I64_EXPORTS]) {
#if CONSUMER_KIND!=2
    XrXirCallEntry observed[128];library_state_task_i64_observe(spec,observed);
#endif
    library_compile_operation_cases("Library private state seal",library_state_task_i64_seal_operation,spec);
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_program_seal(context,spec,&program)==XR_XIR_OK);
    size_t live=runtime_live,bytes=runtime_bytes,sites=0;
    for (size_t p=0;p<=sites;++p) {
        XrXirValue held[8]={0};runtime_attempts=0;runtime_fail_at=p ? p-1 : SIZE_MAX;
        bool ok=library_state_task_i64_pair(program,ids,held);
        if (!p) {CHECK(ok);sites=runtime_attempts;CHECK(sites);} else CHECK(!ok);
        for (uint32_t i=0;i<8;++i) xr_xir_value_drop(&held[i]);
        CHECK(runtime_live==live && runtime_bytes==bytes);
    }
    runtime_fail_at=SIZE_MAX;library_state_task_i64_axes(program,ids);XrXirValue held[8]={0};CHECK(library_state_task_i64_pair(program,ids,held));
    xr_xir_compile_program_drop(program);xr_xir_compile_artifact_free(lowered);
    runtime_attempts=0;runtime_fail_at=0;
    for (uint32_t i=0;i<2;++i) {
        library_state_task_i64_held(&held[i]);library_state_task_i64_task(&held[i+2],1);
        library_state_task_i64_error(&held[i+4]);library_state_task_i64_cancelled(&held[i+6]);
    }
    for (uint32_t i=0;i<8;++i) xr_xir_value_drop(&held[i]);
    CHECK(!runtime_attempts && !runtime_live && !runtime_bytes);runtime_fail_at=SIZE_MAX;
    printf("Library private state twoInstances counter1/2 rootSnapshot1/3 pureWorker41 Task1/repeatedAwait40/preparedChild41/errorOwned/cancelled retainedOwners runtimeOOM=%zu physicalzero\n",sites);
}
#endif
#endif
