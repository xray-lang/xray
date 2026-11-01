/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_task_go_transfer_controls.h - Real parent words before Task publication
 *
 * KEY CONCEPT:
 *   Both compiler adapters execute their original transaction. Faults before
 *   child ownership preserve the actual word, references and scheduler frame.
 */
#ifndef XIR_TASK_GO_TRANSFER_CONTROLS_H
#define XIR_TASK_GO_TRANSFER_CONTROLS_H
static void task_go_after_allocate(void *pointer,size_t bytes);
#define XIR_RUNTIME_AFTER_ALLOCATE(pointer,bytes) task_go_after_allocate(pointer,bytes)
#define xr_xir_task_go_at task_go_transfer_actual
#include "xir_task_initializer_slot_controls.h"
#undef xr_xir_task_go_at
#undef XIR_RUNTIME_AFTER_ALLOCATE
#include "xir/xxir_task_budget.c"
#include "xir/xxir_task.c"
static unsigned task_go_transfers,task_go_refusals,task_go_oom;
static bool task_go_cancel_test;
static unsigned task_go_cancellations,task_go_allocation,task_go_cancel_at;
static XrXirCall *task_go_cancel_parent;
static void task_go_after_allocate(void *pointer,size_t bytes) {
    (void)pointer;(void)bytes;
    if(task_go_cancel_parent&&++task_go_allocation==task_go_cancel_at)
        CHECK(xr_xir_call_request_cancel(task_go_cancel_parent)==XR_XIR_CALL_CANCEL_REQUESTED);
}
XR_FUNC XrXirCallStatus xr_xir_task_go_at(XrXirCallView *view,uint32_t instruction,
    void *frame,const XrXirValue *arguments,uint32_t count,XrXirValue *output) {
    XrXirInstance *instance=view_instance(view);
    uint32_t entry=xr_xir_call_current_entry(view->activation);
    const XrXirModule *module=&instance->program->permissions->input->artifact->module;
    const XrXirFunction *caller=&module->functions[entry];
    const XrXirInstruction *op=&caller->instructions[instruction];
    const XrXirFunction *callee=&module->functions[op->immediate];
    XrXirType type=count?(XrXirType)arguments[0].type:XR_XIR_UNIT;
    if(count!=1||callee->consume_parameter_count!=1||callee->consume_parameters[0]||
        !xr_xir_type_is_array(module->types,type)||xr_xir_array_element(module->types,type)!=XR_XIR_I64)
        return task_go_transfer_actual(view,instruction,frame,arguments,count,output);
    uint32_t first=0,n=0;
    CHECK(xr_xir_direct_argument_range(op,&first,&n)&&n==count);
    const XrXirFunctionLayout *layout=&instance->program->permissions->layouts[entry];
    void *word=(unsigned char *)frame+layout->offsets[caller->operands[first]];
    int64_t original=0,current=0;memcpy(&original,word,sizeof(original));
    CHECK(original==arguments[0].payload);
    XirObject *object=object_pointer(arguments);
    uint32_t references=atomic_load(&object->references);
    XrXirValue refused={0};
    CHECK(task_go_transfer_actual(view,instruction,
        (unsigned char *)view->state+view->activation->top->entry->state_bytes,
        arguments,count,&refused)==XR_XIR_CALL_BAD_ARGUMENT&&!refused.type);
    XrXirValue different={0};const XrXirValue item={XR_XIR_I64,0,7};
    CHECK(xr_xir_array_new(type,&item,1,xr_xir_call_admission(view),&different)==XR_XIR_VALUE_OK);
    CHECK(task_go_transfer_actual(view,instruction,frame,&different,1,&refused)==XR_XIR_CALL_BAD_ARGUMENT&&!refused.type);
    xr_xir_value_drop(&different);task_go_refusals+=2;
    XrXirTaskExecutor *executor=instance->executor;
    for(size_t failure=0;failure<3;++failure) {
        size_t blocks=runtime_live,bytes=runtime_bytes;
        uint64_t identity=executor->next_identity;
        XirTask *active=executor->active;
        XirTaskActivation *head=executor->ready_head,*tail=executor->ready_tail;
        XrCompileResourceStats before,after;
        CHECK(xr_compile_resources_stats(instance->program->context.resources,&before)==XR_COMPILE_RESOURCE_OK);
        runtime_fail_at=runtime_attempts+failure;
        XrXirCallStatus status=task_go_transfer_actual(view,instruction,frame,arguments,count,&refused);
        runtime_fail_at=SIZE_MAX;memcpy(&current,word,sizeof(current));
        CHECK(status==XR_XIR_CALL_OOM&&!refused.type&&current==original);
        CHECK(runtime_live==blocks&&runtime_bytes==bytes&&atomic_load(&object->references)==references);
        CHECK(executor->next_identity==identity&&executor->active==active&&
            executor->ready_head==head&&executor->ready_tail==tail);
        CHECK(xr_compile_resources_stats(instance->program->context.resources,&after)==XR_COMPILE_RESOURCE_OK&&
            after.held_bytes==before.held_bytes);
        ++task_go_oom;
    }
    if(task_go_cancel_test) {
        size_t blocks=runtime_live,bytes=runtime_bytes;
        uint64_t identity=executor->next_identity;
        XirTask *active=executor->active;
        XirTaskActivation *head=executor->ready_head,*tail=executor->ready_tail;
        XrCompileResourceStats before,after;
        CHECK(xr_compile_resources_stats(instance->program->context.resources,&before)==XR_COMPILE_RESOURCE_OK);
        /* This worker prepares an optional member, Task, Call and child frame.
         * The real cancellation precedes the final actual-word recheck. */
        task_go_cancel_at=op->op==XR_XIR_SCOPED_GO?4u:3u;
        task_go_allocation=0;task_go_cancel_parent=view->activation;
        XrXirCallStatus status=task_go_transfer_actual(view,instruction,frame,arguments,count,output);
        task_go_cancel_parent=NULL;memcpy(&current,word,sizeof(current));
        CHECK(task_go_allocation==task_go_cancel_at&&status==XR_XIR_CALL_CANCELLED&&!output->type);
        CHECK(current==original&&atomic_load(&object->references)==references);
        CHECK(runtime_live==blocks&&runtime_bytes==bytes&&executor->next_identity==identity&&
            executor->active==active&&executor->ready_head==head&&executor->ready_tail==tail);
        CHECK(xr_compile_resources_stats(instance->program->context.resources,&after)==XR_COMPILE_RESOURCE_OK&&
            after.held_bytes==before.held_bytes);
        ++task_go_cancellations;return status;
    }
    XrXirCallStatus status=task_go_transfer_actual(view,instruction,frame,arguments,count,output);
    memcpy(&current,word,sizeof(current));
    CHECK(status==XR_XIR_CALL_READY&&output->type&&!current&&atomic_load(&object->references)==references);
    ++task_go_transfers;return status;
}
#endif // XIR_TASK_GO_TRANSFER_CONTROLS_H
