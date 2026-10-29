/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_task_result.inc.c - Failure-atomic snapshots of sticky task outcomes
 */
XR_FUNC XrXirCallStatus xir_task_result_wrap(const XrXirCallResult *outcome,
    XrXirType type,XrXirValueAdmission *admission,XrXirValue *output) {
    if (!outcome || !admission || !task_core_empty_value(output) ||
        !xr_xir_call_result_valid(outcome)) return XR_XIR_CALL_BAD_ARGUMENT;
    const XrXirTypes *types=xr_xir_compile_type_arena_types(admission->arena);
    XrXirTaskResultType shape={0};
    if (!xr_xir_task_result_type_info(types,type,&shape)) return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirValue payload={0};const XrXirValue *field=NULL;uint32_t variant=0;
    XrXirValueStatus status=XR_XIR_VALUE_OK;
    if (outcome->status==XR_XIR_CALL_RETURNED) {
        variant=XR_XIR_TASK_RESULT_SUCCESS;field=&outcome->value;
        if (shape.element==XR_XIR_UNIT ? !task_core_empty_value(field) :
            !xr_xir_value_argument(field,admission->arena,shape.element)) return XR_XIR_CALL_BAD_STATE;
    } else if (outcome->status==XR_XIR_CALL_THROWN) {
        variant=XR_XIR_TASK_RESULT_ERROR;
        if (shape.error==XR_XIR_NEVER) return XR_XIR_CALL_BAD_STATE;
        status=xr_xir_error_erase(&outcome->value,admission,&payload);
        if (status==XR_XIR_VALUE_OK) {
            XrXirValue concrete={0};XrXirType local=XR_XIR_UNIT;
            if (!xr_xir_error_borrow(&payload,&concrete)) status=XR_XIR_VALUE_BAD_ARGUMENT;
            const XrXirTypeArena *owner=xr_xir_value_arena(&concrete);
            if (status==XR_XIR_VALUE_OK && owner!=admission->arena) {
                uint64_t work=xir_type_arena_lookup_work(admission->arena);
                if (work>admission->work || !xr_xir_domain_work(admission->domain,work)) status=XR_XIR_VALUE_LIMIT;
                else admission->work-=work;
            }
            if (status==XR_XIR_VALUE_OK && (!xir_type_arena_local_type(admission->arena,owner,
                (XrXirType)concrete.type,&local) || !xr_xir_error_bound_accepts(types,local,shape.error)))
                status=XR_XIR_VALUE_BAD_ARGUMENT;
        }
        field=&payload;
    } else if (xr_xir_call_panic_status(outcome->status)) {
        variant=XR_XIR_TASK_RESULT_PANIC;
        status=xr_xir_panic_payload_admit(&outcome->panic,admission);
        if (status==XR_XIR_VALUE_OK) status=xr_xir_panic_info_new(admission->domain,&outcome->panic,&payload);
        field=&payload;
    } else if (outcome->status==XR_XIR_CALL_CANCELLED) variant=XR_XIR_TASK_RESULT_CANCELLED;
    else return outcome->status==XR_XIR_CALL_CLEANUP_FAILED || outcome->status==XR_XIR_CALL_READY || outcome->status==XR_XIR_CALL_SUSPENDED ||
        outcome->status==XR_XIR_CALL_YIELDED || outcome->status==XR_XIR_CALL_CONSUMED ?
        XR_XIR_CALL_BAD_STATE:outcome->status;
    if (status==XR_XIR_VALUE_OK) status=xr_xir_enum_new(type,variant,field,field?1u:0u,admission,output);
    xr_xir_value_drop(&payload);return task_core_value_status(status);
}

static XrXirCallStatus task_result_subject(XrXirTaskExecutor *executor,
    XrXirCallView *view,const XrXirValue *value,bool *terminal) {
    bool child=false;
    if (!executor || !value || !terminal ||
        !xr_xir_task_executor_view_member(executor,view,&child) || task_executor_observing(executor))
        return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirValueAdmission *admission=xr_xir_call_admission(view);
    if (!admission || admission->domain!=executor->config.domain ||
        admission->arena!=executor->config.program->arena ||
        !xr_xir_value_argument(value,admission->arena,(XrXirType)value->type)) return XR_XIR_CALL_BAD_ARGUMENT;
    const XrXirTypeNode *node=xr_xir_type_node(executor->config.program->types,(XrXirType)value->type);
    if (!node || node->kind!=XR_XIR_TYPE_TASK) return XR_XIR_CALL_BAD_ARGUMENT;
    const XirTask *target=(const XirTask *)(uintptr_t)value->payload;
    if (target->object.domain!=executor->config.domain || target->generation!=executor->generation ||
        !target->identity || target->identity>executor->next_identity) return XR_XIR_CALL_BAD_ARGUMENT;
    if (!task_pending_member_work(admission,1)) return XR_XIR_CALL_LIMIT;
    if (atomic_load_explicit(&target->state,memory_order_acquire)==XIR_TASK_TERMINAL) {
        if (target->outcome.status==XR_XIR_CALL_CLEANUP_FAILED) return XR_XIR_CALL_BAD_STATE;
        *terminal=true;return XR_XIR_CALL_READY;
    }
    uint64_t walked=0;
    for (const XirTask *task=executor->active;task;task=task->active_next) {
        if (++walked>executor->next_identity) return XR_XIR_CALL_BAD_STATE;
        if (!task_pending_member_work(admission,1)) return XR_XIR_CALL_LIMIT;
        if (task!=target) continue;
        const XirTaskActivation *activation=&task->activation;
        const XrXirExecutorBinding binding={executor,(void *)activation,executor->generation,task->identity};
        if (task->executor!=executor || !task->call || activation->task!=task ||
            activation->call!=task->call || activation->ticket!=task->identity ||
            !xr_xir_call_executor_member(task->call,&binding)) return XR_XIR_CALL_BAD_STATE;
        *terminal=false;return XR_XIR_CALL_READY;
    }
    return XR_XIR_CALL_BAD_STATE;
}
XR_FUNC XrXirCallStatus xir_task_executor_result_member(XrXirTaskExecutor *executor,
    XrXirCallView *view,const XrXirValue *value) {
    bool terminal=false;return task_result_subject(executor,view,value,&terminal);
}
XR_FUNC XrXirCallStatus xir_task_executor_observe(XrXirTaskExecutor *executor,
    XrXirCallView *view,const XrXirValue *value,bool *terminal,XrXirCallResult *output) {
    if (!terminal || !xr_xir_call_result_empty(output)) return XR_XIR_CALL_BAD_ARGUMENT;
    bool finished=false;XrXirCallStatus status=task_result_subject(executor,view,value,&finished);
    if (status!=XR_XIR_CALL_READY) return status;
    if (finished) {
        XrXirCallResult prepared={0};status=xr_xir_task_copy_outcome(value,&prepared);
        if (xr_xir_call_result_empty(&prepared)) return status;
        xr_xir_call_result_move(&prepared,output);
    }
    *terminal=finished;return XR_XIR_CALL_READY;
}
