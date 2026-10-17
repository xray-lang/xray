/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_native_task_hold.c - Actual child throws, siblings and cleanup ownership
 */
#include "xir/xxir_program.h"
#include "xir/xxir_task.h"
#include "xir/xxir_error.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"native Task hold %d: %s\n",__LINE__,#c);exit(1); } } while(0)
#include "../../xir/xir_go_source_native_compile_owner.h"
#include "../../xir/xir_runtime_allocations.h"
#include "xir/xxir_task.c"
#include "xir/xxir_call_native_observer_internal.h"
#include "xir/xxir_call_native_source_internal.h"
#include "native_task.h"

_Static_assert(NT_FUNCTIONS<=128,"finite native fixture observation capacity");
enum { NT_CONTINUE,NT_CANCEL_ROOT,NT_CANCEL_CHILD,NT_STOP,NT_FREE };
enum { NT_NATIVE,NT_VM_NATIVE,NT_NATIVE_VM };
typedef struct NativeTaskRecord {
    XrXirInstance *instance;
    XirCallNativeObserver observer;
    const XrXirCallView *view;
    XirCallNativeFrame held;
    XrXirValue task;
    unsigned steps[NT_FUNCTIONS],exits[NT_FUNCTIONS],releases[NT_FUNCTIONS],throws[NT_FUNCTIONS],leaves[NT_FUNCTIONS];
    XrXirCallStatus reasons[NT_FUNCTIONS];
    int64_t output[16];unsigned outputs,notifications;
    XrXirCallStatus rejection;
    bool enabled,captured,pause_requested;
} NativeTaskRecord;
typedef struct NativeTaskFixture {
    NativeTaskRecord records[2];
    const XrXirCompileContext *context;
    XrXirCallEntry *entries;
    XrXirCallEntry *selected;
    XrXirVmBinding *bindings;
    XrXirArtifact *lowered;
    unsigned backend;
    unsigned code_releases;
} NativeTaskFixture;
static NativeTaskFixture *native_tasks;

static NativeTaskRecord *task_record(const void *instance) {
    CHECK(native_tasks);
    for(unsigned i=0;i<2;++i) if(native_tasks->records[i].instance==instance) return &native_tasks->records[i];
    CHECK(false);return NULL;
}
static void task_error(const XrXirValue *value) {
    XrXirValue concrete=*value;
    if(concrete.type==XR_XIR_ERROR) CHECK(xr_xir_error_borrow(value,&concrete));
    XrXirEnumBorrow error={0};CHECK(xr_xir_enum_borrow(&concrete,&error)==XR_XIR_VALUE_OK);
    CHECK(error.name.length==7 && !memcmp(error.name.bytes,"Problem",7) && error.member.length==6 &&
        !memcmp(error.member.bytes,"Failed",6) && error.field_count==1);
    const char expected[]="child\0\xCE\xB1";const char *bytes=NULL;size_t length=0;
    CHECK(xr_xir_string_view(&error.fields[0],&bytes,&length) && length==sizeof(expected)-1 && !memcmp(bytes,expected,length));
}
static XrXirCallStatus task_throw(void *owner,const XirCallNativeThrowEvent *event,bool *pause) {
    NativeTaskRecord *w=owner;CHECK(w && event && pause && event->frame.instance==w->instance);
    *pause=false;
    if(event->frame.entry!=NT_FAIL) return XR_XIR_CALL_READY;
    ++w->notifications;task_error(event->value);
    const XirTaskActivation *activation=event->frame.binding.activation;
    CHECK(activation->task && activation->call && activation->call!=w->instance->call);
    CHECK(xr_xir_call_executor_member(activation->call,&event->frame.binding));
    CHECK(event->frame.phase==XR_XIR_CALL_NORMAL && !event->frame.parent && event->frame.depth==1);
    CHECK(w->view && !xr_xir_call_admission(w->view));
    CHECK(xr_xir_instance_poll_bounded(w->instance,1).outcome.status==XR_XIR_CALL_BUSY);
    CHECK(xr_xir_instance_cancel_current(w->instance)==XR_XIR_CALL_BUSY);
    CHECK(xr_xir_instance_stop(w->instance)==XR_XIR_CALL_BUSY && xr_xir_instance_free(w->instance)==XR_XIR_CALL_BUSY);
    CHECK(xr_xir_call_request_cancel(activation->call)==XR_XIR_CALL_BUSY);
    if(!w->captured) {CHECK(event->frame.scope_exit);w->held=event->frame;w->captured=true;*pause=w->pause_requested;}
    else CHECK(!event->frame.scope_exit);
    w->view=NULL;return w->rejection;
}
static XrXirAction task_resume(XrXirCallView *view) {
    NativeTaskRecord *w=task_record(view->instance);uint32_t f=xr_xir_call_current_entry(view->activation);
    CHECK(f<NT_FUNCTIONS && !w->view && view->environment==native_tasks->selected[f].environment);
    if(w->enabled) CHECK(xir_call_native_observer_bind(view,&w->observer)==XR_XIR_CALL_READY);
    if(view->phase==XR_XIR_CALL_EXIT) ++w->exits[f];else ++w->steps[f];
    if(f==NT_FAIL && native_tasks->backend!=NT_NATIVE_VM && view->phase==XR_XIR_CALL_NORMAL) {
        bool unchanged=true;XrXirCallView copy=*view;
        CHECK(xir_call_native_source_before(&copy,0,view->state,0,&unchanged)==XR_XIR_CALL_BAD_ARGUMENT && unchanged);
        const unsigned char *outside=(const unsigned char *)((uintptr_t)view->state-1);
        CHECK(xir_call_native_source_before(view,0,outside,0,&unchanged)==XR_XIR_CALL_BAD_STATE && unchanged);
        CHECK(xir_call_native_source_before(view,0,view->state,UINT32_MAX,&unchanged)==XR_XIR_CALL_BAD_STATE && unchanged);
    }
    w->view=view;XrXirAction action=native_tasks->selected[f].resume(view);
    if(view->phase==XR_XIR_CALL_NORMAL) {
        w->throws[f]+=action.kind==XR_XIR_ACTION_THROW;
        w->leaves[f]+=action.kind==XR_XIR_ACTION_LEAVE && action.flags==XR_XIR_ACTION_LEAVE_ERROR;
    }
    const XrXirTypeNode *type=xr_xir_type_node(native_task_program.types,(XrXirType)action.value.type);
    if(!w->task.type && (action.kind==XR_XIR_ACTION_AWAIT_TASK ||
        (action.kind==XR_XIR_ACTION_RETURN && type && type->kind==XR_XIR_TYPE_TASK))) {
        CHECK(xr_xir_value_copy(&action.value,&w->task)==XR_XIR_VALUE_OK);
    }
    if(action.kind!=XR_XIR_ACTION_THROW && !(action.kind==XR_XIR_ACTION_LEAVE && action.flags==XR_XIR_ACTION_LEAVE_ERROR)) w->view=NULL;
    return action;
}
static void task_release(XrXirCallView *view,XrXirCallStatus reason) {
    NativeTaskRecord *w=task_record(view->instance);
    uint32_t f=(uint32_t)(view->activation->top->entry-view->activation->config.entries);CHECK(f<NT_FUNCTIONS);
    ++w->releases[f];w->reasons[f]=reason;CHECK(!xr_xir_call_admission(view));
    native_tasks->selected[f].release(view,reason);
}
static void task_code_release(void *owner) {
    NativeTaskFixture *f=owner;++f->code_releases;xr_xir_compile_artifact_free(f->lowered);f->lowered=NULL;
}
static XrXirOutputStatus task_output(void *owner,const XrXirOutputGroup *group) {
    NativeTaskRecord *w=owner;CHECK(group && group->stream==XR_XIR_STDOUT && group->line && group->count==1);
    CHECK(group->values[0].type==XR_XIR_I64 && w->outputs<16);w->output[w->outputs++]=group->values[0].payload;
    return XR_XIR_OUTPUT_OK;
}
static void task_fixture(NativeTaskFixture *f,unsigned count,bool observed,unsigned backend) {
    memset(f,0,sizeof(*f));native_tasks=f;f->context=effects_source_owner(67108864,128000000);
    f->backend=backend;
    CHECK(native_task_program.entry_count==NT_FUNCTIONS);
    CHECK(xr_compile_resources_calloc(f->context->resources,NT_FUNCTIONS,sizeof(*f->entries),(void **)&f->entries)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_resources_calloc(f->context->resources,NT_FUNCTIONS,sizeof(*f->selected),(void **)&f->selected)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_resources_calloc(f->context->resources,NT_FUNCTIONS,sizeof(*f->bindings),(void **)&f->bindings)==XR_COMPILE_RESOURCE_OK);
    memcpy(f->selected,native_task_program.entries,NT_FUNCTIONS*sizeof(*f->selected));
    XrXirProgramSpec spec=native_task_program;
    if(backend!=NT_NATIVE) {
        XrXirArtifact *read=NULL,*closed=NULL;
        CHECK(xr_xir_compile_checked_read(f->context,spec.proof.bytes,spec.proof.length,&read,NULL)==XR_XIR_OK);
        CHECK(xr_xir_compile_specialize(read,&closed,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(read);
        CHECK(xr_xir_compile_lower(closed,&spec.target,&f->lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
        const XrXirModule *module=xr_xir_compile_artifact_module(f->lowered);CHECK(module->function_count==NT_FUNCTIONS);
        for(uint32_t i=0;i<NT_FUNCTIONS;++i) if((backend==NT_VM_NATIVE && i!=NT_FAIL) || (backend==NT_NATIVE_VM && i==NT_FAIL))
            CHECK(xr_xir_compile_vm_bind(f->lowered,i,&f->bindings[i],&f->selected[i])==XR_XIR_OK);
        spec.types=module->types;spec.declarations=module->declarations;spec.proof=xr_xir_compile_program_proof(f->lowered);
    }
    memcpy(f->entries,f->selected,NT_FUNCTIONS*sizeof(*f->entries));
    for(unsigned i=0;i<NT_FUNCTIONS;++i) {f->entries[i].resume=task_resume;f->entries[i].release=task_release;}
    spec.entries=f->entries;spec.code=(XrXirCodeLease){f,task_code_release};
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_program_seal(f->context,&spec,&program)==XR_XIR_OK);
    for(unsigned i=0;i<count;++i) {
        NativeTaskRecord *w=&f->records[i];w->enabled=observed;w->pause_requested=true;w->observer=(XirCallNativeObserver){w,task_throw};
        XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
        config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,task_output,w};
        CHECK(xr_xir_instance_new(program,&config,&w->instance)==XR_XIR_CALL_READY);
    }
    xr_xir_compile_program_drop(program);CHECK(!f->code_releases);
}
static XrXirInstanceResult task_poll(NativeTaskRecord *w,bool hold) {
    XrXirInstanceResult result={0};
    for(unsigned i=0;i<20000;++i) {
        w->view=NULL;result=xr_xir_instance_poll_bounded(w->instance,1);
        if((hold && w->instance->budget.native_hold) || result.outcome.status!=XR_XIR_CALL_READY) return result;
    }
    CHECK(false);return result;
}
static void task_hold(NativeTaskRecord *w,uint32_t entry,XirCallNativeThrow *copy) {
    CHECK(xr_xir_instance_start(w->instance,entry,NULL,0)==XR_XIR_CALL_READY);
    XrXirInstanceResult stopped=task_poll(w,true);
    fprintf(stderr,"child hold observation: entry=%u status=%u captured=%u notifications=%u outputs=%u exits=%u releases=%u\n",
        entry,stopped.outcome.status,w->captured,w->notifications,w->outputs,w->exits[NT_FAIL],w->releases[NT_FAIL]);
    CHECK(stopped.outcome.status==XR_XIR_CALL_READY && w->captured);
    XrXirCall *held=w->instance->budget.native_hold;CHECK(held && held!=w->instance->call);
    CHECK(!w->outputs && !w->releases[NT_FAIL] && !w->exits[NT_FAIL]);
    if(entry!=NT_ESCAPING) CHECK(!w->releases[NT_SIBLING] && !w->exits[NT_SIBLING] && !w->releases[entry]);
    else CHECK(w->releases[NT_ESCAPING]==1 && xr_xir_call_state(w->instance->call)==XR_XIR_CALL_RETURNED);
    NativeTaskRecord before=*w;XrXirDomainBudgetStats budget=xr_xir_domain_budget_stats(w->instance->domain);
    size_t allocations=runtime_attempts;
    for(unsigned i=0;i<16;++i) CHECK(xr_xir_instance_poll_bounded(w->instance,UINT64_MAX).outcome.status==XR_XIR_CALL_READY);
    CHECK(!memcmp(before.steps,w->steps,sizeof(w->steps)) && !memcmp(before.exits,w->exits,sizeof(w->exits)) &&
        !memcmp(before.releases,w->releases,sizeof(w->releases)) && !w->outputs && runtime_attempts==allocations);
    CHECK(xr_xir_domain_budget_stats(w->instance->domain).work==budget.work);
    CHECK(xir_call_native_throw_copy(held,&w->held.binding,w->held.frame,copy)==XR_XIR_CALL_READY);task_error(&copy->value);
    CHECK(copy->frame.scope_exit && w->leaves[NT_FAIL]==1 && !w->throws[NT_FAIL]);
    if(w->task.type) {XrXirCallResult outcome={0};CHECK(xr_xir_task_copy_outcome(&w->task,&outcome)==XR_XIR_CALL_BAD_STATE && xr_xir_call_result_empty(&outcome));}
}
static unsigned task_count_output(const NativeTaskRecord *w,int64_t value) {
    unsigned count=0;for(unsigned i=0;i<w->outputs;++i) count+=w->output[i]==value;return count;
}
static void task_finish(NativeTaskRecord *w,uint32_t entry,unsigned finish) {
    XrXirCall *held=w->instance->budget.native_hold;CHECK(held);
    unsigned fail_steps=w->steps[NT_FAIL];
    if(finish==NT_CANCEL_ROOT) {
        CHECK(xr_xir_instance_cancel_current(w->instance)==(entry==NT_ESCAPING ? XR_XIR_CALL_BAD_STATE : XR_XIR_CALL_CANCEL_REQUESTED));
        if(entry==NT_ESCAPING) {CHECK(w->instance->budget.native_hold==held);CHECK(xir_call_native_throw_continue(held,&w->held.binding,w->held.frame)==XR_XIR_CALL_READY);}
    } else if(finish==NT_CANCEL_CHILD) CHECK(xr_xir_call_request_cancel(held)==XR_XIR_CALL_CANCEL_REQUESTED);
    else if(finish==NT_STOP) CHECK(xr_xir_instance_stop(w->instance)==XR_XIR_CALL_READY);
    else if(finish==NT_FREE) {
        CHECK(xr_xir_instance_free(w->instance)==XR_XIR_CALL_READY);w->instance=NULL;
    } else CHECK(xir_call_native_throw_continue(held,&w->held.binding,w->held.frame)==XR_XIR_CALL_READY);
    if(w->instance) {
        XrXirInstanceResult result=task_poll(w,false);
        XrXirCallStatus expected=finish==NT_CONTINUE || (finish==NT_CANCEL_ROOT && entry==NT_ESCAPING) ?
            XR_XIR_CALL_RETURNED : XR_XIR_CALL_CANCELLED;
        CHECK(result.outcome.status==expected);
        if(expected==XR_XIR_CALL_RETURNED && entry!=NT_ESCAPING) CHECK(result.outcome.value.type==XR_XIR_I64 && result.outcome.value.payload==(entry==NT_AWAITING ? 17 : 8));
        CHECK(!w->instance->budget.native_hold);CHECK(xr_xir_instance_free(w->instance)==XR_XIR_CALL_READY);w->instance=NULL;
    }
    CHECK(w->steps[NT_FAIL]>=fail_steps && w->releases[NT_FAIL]==1 && task_count_output(w,89)==1 && task_count_output(w,91)==1);
    CHECK(w->reasons[NT_FAIL]==(finish==NT_CANCEL_CHILD || finish==NT_STOP || finish==NT_FREE ? XR_XIR_CALL_CANCELLED : XR_XIR_CALL_THROWN));
    CHECK(w->leaves[NT_FAIL]==1 && w->throws[NT_FAIL]==(finish==NT_CANCEL_CHILD || finish==NT_STOP || finish==NT_FREE ? 0u : 1u));
    if(entry!=NT_ESCAPING) {
        CHECK(task_count_output(w,73)==1 && task_count_output(w,95)==1 && w->releases[NT_SIBLING]==1);
        CHECK(task_count_output(w,17)==(finish==NT_STOP || finish==NT_FREE ? 0u : 1u));
    }
    if(finish==NT_CONTINUE && entry==NT_AWAITING) {const int64_t ordered[]={89,91,17,73,95};CHECK(w->outputs==5 && !memcmp(w->output,ordered,sizeof(ordered)));}
    if(w->task.type) {
        for(unsigned i=0;i<2;++i) {
            XrXirCallResult outcome={0};XrXirCallStatus expected=finish==NT_CANCEL_CHILD || finish==NT_STOP || finish==NT_FREE ? XR_XIR_CALL_CANCELLED : XR_XIR_CALL_THROWN;
            CHECK(xr_xir_task_copy_outcome(&w->task,&outcome)==expected);
            if(expected==XR_XIR_CALL_THROWN) task_error(&outcome.value);
            xr_xir_call_result_drop(&outcome);
        }
        xr_xir_value_drop(&w->task);
    }
    printf("NATIVE_TASK_HOLD entry=%u finish=%u normalSteps=%u childReleases=1 outputs=%u\n",entry,finish,w->steps[NT_FAIL],w->outputs);
}
static void task_close(NativeTaskFixture *f,XirCallNativeThrow copies[2]) {
    CHECK(f->code_releases==1);
    for(unsigned i=0;i<2;++i) if(copies[i].value.type) {task_error(&copies[i].value);xr_xir_value_drop(&copies[i].value);}
    xr_compile_resources_free(f->entries);xr_compile_resources_free(f->selected);xr_compile_resources_free(f->bindings);
    CHECK(!f->lowered && !runtime_live && !runtime_bytes);
    effects_source_owners_free();native_tasks=NULL;
}
static void task_case(uint32_t entry,unsigned finish,unsigned backend) {
    NativeTaskFixture f;task_fixture(&f,1,true,backend);XirCallNativeThrow copies[2]={{0}};
    task_hold(&f.records[0],entry,&copies[0]);task_finish(&f.records[0],entry,finish);task_close(&f,copies);
}
static void task_isolation(void) {
    NativeTaskFixture f;task_fixture(&f,2,true,NT_VM_NATIVE);XirCallNativeThrow copies[2]={{0}};
    for(unsigned i=0;i<2;++i) task_hold(&f.records[i],NT_AWAITING,&copies[i]);
    NativeTaskRecord *a=&f.records[0],*b=&f.records[1];XirCallNativeThrow foreign={0};
    CHECK(xir_call_native_throw_copy(a->instance->budget.native_hold,&b->held.binding,b->held.frame,&foreign)==XR_XIR_CALL_BAD_STATE && !foreign.value.type);
    CHECK(a->instance->domain!=b->instance->domain);
    task_finish(a,NT_AWAITING,NT_CANCEL_ROOT);CHECK(!f.code_releases && b->instance->budget.native_hold);
    task_finish(b,NT_AWAITING,NT_CONTINUE);task_close(&f,copies);
}
static void task_unpaused(unsigned backend,bool observed) {
    NativeTaskFixture f;task_fixture(&f,1,observed,backend);NativeTaskRecord *w=&f.records[0];
    w->pause_requested=false;CHECK(xr_xir_instance_start(w->instance,NT_AWAITING,NULL,0)==XR_XIR_CALL_READY);
    XrXirInstanceResult result=task_poll(w,false);CHECK(result.outcome.status==XR_XIR_CALL_RETURNED && result.outcome.value.payload==17);
    CHECK(!w->instance->budget.native_hold && w->notifications==(observed && backend!=NT_NATIVE_VM ? 2u : 0u));
    const int64_t ordered[]={89,91,17,73,95};CHECK(w->outputs==5 && !memcmp(w->output,ordered,sizeof(ordered)));
    CHECK(xr_xir_instance_free(w->instance)==XR_XIR_CALL_READY);w->instance=NULL;
    XrXirCallResult outcome={0};CHECK(xr_xir_task_copy_outcome(&w->task,&outcome)==XR_XIR_CALL_THROWN);task_error(&outcome.value);
    xr_xir_call_result_drop(&outcome);xr_xir_value_drop(&w->task);XirCallNativeThrow copies[2]={{0}};task_close(&f,copies);
    printf("NATIVE_TASK_UNPAUSED backend=%u observer=%u originalOutput=89/91/17/73/95 physical=0/0\n",backend,observed);
}
static void task_rejection(unsigned backend,XrXirCallStatus rejection) {
    NativeTaskFixture f;task_fixture(&f,1,true,backend);NativeTaskRecord *w=&f.records[0];w->rejection=rejection;
    CHECK(xr_xir_instance_start(w->instance,NT_AWAITING,NULL,0)==XR_XIR_CALL_READY);
    XrXirInstanceResult result=task_poll(w,false);
    XrXirCallStatus expected=rejection==XR_XIR_CALL_BAD_ARGUMENT ? XR_XIR_CALL_BAD_STATE : rejection;
    CHECK(result.outcome.status==expected && w->notifications==1 && !w->instance->budget.native_hold);
    CHECK(w->releases[NT_FAIL]==1 && w->reasons[NT_FAIL]==expected && w->leaves[NT_FAIL]==1 && !w->throws[NT_FAIL]);
    unsigned cleanup=rejection==XR_XIR_CALL_BAD_ARGUMENT ? 0u : 1u;
    CHECK(task_count_output(w,89)==cleanup && task_count_output(w,91)==cleanup);
    CHECK(xr_xir_instance_free(w->instance)==expected);w->instance=NULL;
    XrXirCallResult outcome={0};CHECK(xr_xir_task_copy_outcome(&w->task,&outcome)==expected && outcome.status==expected);
    xr_xir_call_result_drop(&outcome);xr_xir_value_drop(&w->task);XirCallNativeThrow copies[2]={{0}};task_close(&f,copies);
    printf("NATIVE_TASK_OBSERVER_REJECT backend=%u input=%u status=%u childCleanup=%u stickyFailure=1 physical=0/0\n",backend,rejection,expected,cleanup);
}
int main(void) {
    for(unsigned backend=NT_NATIVE;backend<=NT_VM_NATIVE;++backend) {
        for(unsigned finish=NT_CONTINUE;finish<=NT_FREE;++finish) task_case(NT_AWAITING,finish,backend);
        task_case(NT_BUSY,NT_CONTINUE,backend);task_case(NT_BUSY,NT_CANCEL_ROOT,backend);
        task_case(NT_ESCAPING,NT_CONTINUE,backend);task_case(NT_ESCAPING,NT_CANCEL_ROOT,backend);
    }
    task_unpaused(NT_NATIVE,false);task_unpaused(NT_NATIVE,true);
    task_unpaused(NT_VM_NATIVE,true);task_unpaused(NT_NATIVE_VM,true);
    for(unsigned backend=NT_NATIVE;backend<=NT_VM_NATIVE;++backend) {
        task_rejection(backend,XR_XIR_CALL_OOM);task_rejection(backend,XR_XIR_CALL_LIMIT);task_rejection(backend,XR_XIR_CALL_BAD_ARGUMENT);
    }
    task_isolation();puts("Actual Source/native child hold, sibling all-stop, root/child cancellation, sticky owned outcome and physical zero: PASS");return 0;
}
