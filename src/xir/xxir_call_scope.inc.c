/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_call_scope.inc.c - Lexical joins inside the existing cleanup frontier
 *
 * KEY CONCEPT:
 *   The driver parks before the registered helper, never inside a cleanup
 *   callback. Pending exits and their owners remain in the original frame.
 */
static bool call_scope_cleanup_wait(XrXirCall *call,const XrXirAction *action) {
    CallFrame *frame=call->top;
    if(!call->executor_owner||!frame->cleanup_frontier)return false;
    if(action->argument_count||action->arguments||call->next_wake==UINT64_MAX) {
        abort_frames(call,XR_XIR_CALL_BAD_STATE);return true;
    }
    XrXirValue completion={0};
    XrXirCallStatus status=xir_task_scope_cleanup((XrXirTaskExecutor *)call->executor_owner,
        call->active_view,&completion);
    if(status!=XR_XIR_CALL_READY) {
        if(status==XR_XIR_CALL_OOM||status==XR_XIR_CALL_LIMIT)request_execution_failure(call,status);
        else abort_frames(call,status);
        return true;
    }
    if(!completion.type)return false;
    XirTask *task=(XirTask *)(uintptr_t)completion.payload;
    XR_CHECK(!call->waiting_task.type,"a frontier parks on only its own current group");
    call->waiting_task=completion;
    frame->scope_draining=true;frame->scope_cleanup_callee=action->callee;
    call->result=call_result(XR_XIR_CALL_SUSPENDED);call->result.wake=++call->next_wake;
    call->wait=(XrXirWaitRequest){XR_XIR_WAIT_TASK,0,0,task,task->generation,task->identity};
    return true;
}
static void call_scope_cleanup_complete(XrXirCall *call,XrXirCallResult *outcome,uint64_t failure_order) {
    CallFrame *frame=call->top;
    frame->scope_draining=false;frame->scope_resume=true;
    if(call->cancel_requested&&!xr_xir_call_panic_status(frame->pending.status)&&
        call->abort_reason==XR_XIR_CALL_READY) {
        xr_xir_call_result_drop(&frame->pending);frame->pending=call_result(XR_XIR_CALL_CANCELLED);
    }
    if(outcome->status==XR_XIR_CALL_RETURNED) {xr_xir_call_result_drop(outcome);return;}
    if(xr_xir_call_panic_status(outcome->status)) {
        if(frame->pending.status==XR_XIR_CALL_CANCELLED||
            (xr_xir_call_panic_status(frame->pending.status)&&frame->scope_failure_order&&
             frame->scope_failure_order<failure_order)) {
            xr_xir_call_result_drop(outcome);return;
        }
        if(frame->pending.status==XR_XIR_CALL_THROWN) {
            XrXirValue error={0};call->admitting=true;
            XrXirValueStatus status=xr_xir_error_erase(&frame->pending.value,&call->config.admission,&error);
            call->admitting=false;
            if(status!=XR_XIR_VALUE_OK) {
                xr_xir_call_result_drop(outcome);
                request_execution_failure(call,status==XR_XIR_VALUE_OOM?XR_XIR_CALL_OOM:XR_XIR_CALL_LIMIT);return;
            }
            xr_xir_value_drop(&outcome->panic.replaced_error);
            xr_xir_value_move(&error,&outcome->panic.replaced_error);
        }
        xr_xir_call_result_drop(&frame->pending);
        xr_xir_call_result_move(outcome,&frame->pending);frame->scope_failure_order=failure_order;return;
    }
    XrXirCallStatus failure=outcome->status;xr_xir_call_result_drop(outcome);
    request_execution_failure(call,failure);
}
