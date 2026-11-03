/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_task_aggregate.inc.c - Owned observation without moving scope members
 *
 * KEY CONCEPT:
 *   A selected result is prepared before peer cancellation and publication.
 *   Subscriptions survive early publication until the original tasks finish.
 */
static void task_aggregate_member(XirTaskGroup *group,XirTaskSubscription *member) {
    if(group->published)return;
    if(group->kind==XIR_TASK_AGGREGATE_ALL&&member->task->outcome.status==XR_XIR_CALL_RETURNED)return;
    if(!group->winner||member->task->terminal_order<group->winner->task->terminal_order||
        (member->task->terminal_order==group->winner->task->terminal_order&&member->ordinal<group->winner->ordinal))
        group->winner=member;
}
static XrXirValueStatus task_aggregate_array(XirTaskGroup *group,XrXirValue *output) {
    XrXirTaskExecutor *executor=group->executor;
    const XrXirTypeNode *task=xr_xir_type_node(executor->config.program->types,group->completion->object.type);
    XrXirValueAdmission admission={executor->config.task_arena,executor->config.domain,NULL,NULL,
        executor->config.poll_limit,executor->config.call_limit};
    XrXirValueStatus status=XR_XIR_VALUE_OK;uint64_t bytes=group->count*sizeof(XrXirValue);
    XrXirValue *values=bytes?xr_xir_domain_metadata_allocate(executor->config.domain,bytes,&status):NULL;
    if(bytes&&!values)return status;
    for(XirTaskSubscription *member=group->first;member;member=member->next_member)
        values[member->ordinal]=member->task->outcome.value;
    status=xr_xir_array_new(task->element,values,(size_t)group->count,&admission,output);
    if(values)xr_xir_domain_metadata_deallocate(executor->config.domain,values,bytes);
    return status;
}
static void task_aggregate_complete(XirTaskGroup *group) {
    if(!group->sealed||group->published||group->cancel_requested)return;
    if(!group->winner&&group->pending)return;
    XrXirCallResult result={0};XrXirValueStatus prepared=XR_XIR_VALUE_OK;
    if(group->winner)prepared=xr_xir_call_result_copy(&group->winner->task->outcome,&result);
    else {
        XR_CHECK(group->kind==XIR_TASK_AGGREGATE_ALL,"a nonempty first group selects an actual terminal member");
        result.status=XR_XIR_CALL_RETURNED;prepared=task_aggregate_array(group,&result.value);
    }
    if(prepared!=XR_XIR_VALUE_OK) {
        xr_xir_call_result_drop(&result);result.status=task_core_value_status(prepared);
        task_core_record_shutdown(group->executor,&result);
    }
    if(group->winner)task_group_cancel_members(group);
    if(result.status==XR_XIR_CALL_CANCELLED&&group->winner)
        group->completion->cancel_scope=group->winner->task->cancel_scope;
    task_group_publish(group,&result);
}
static XrXirCallStatus task_aggregate_shape(XrXirTaskExecutor *executor,const XrXirValue *array,
    XrXirType result,XirTaskAggregateKind kind) {
    const XrXirTypes *types=executor->config.program->types;
    const XrXirTypeNode *input=xr_xir_type_node(types,(XrXirType)array->type);
    const XrXirTypeNode *member=input&&input->kind==XR_XIR_TYPE_ARRAY?xr_xir_type_node(types,input->element):NULL;
    const XrXirTypeNode *task=xr_xir_type_node(types,result);
    if((kind!=XIR_TASK_AGGREGATE_ALL&&kind!=XIR_TASK_AGGREGATE_FIRST)||!member||
        member->kind!=XR_XIR_TYPE_TASK||member->parameter_span||!task||task->kind!=XR_XIR_TYPE_TASK||
        task->parameter_span||task->result!=member->result)return XR_XIR_CALL_BAD_ARGUMENT;
    if(kind==XIR_TASK_AGGREGATE_FIRST)return task->element==member->element?XR_XIR_CALL_READY:XR_XIR_CALL_BAD_ARGUMENT;
    const XrXirTypeNode *values=xr_xir_type_node(types,task->element);
    return values&&values->kind==XR_XIR_TYPE_ARRAY&&values->element==member->element?
        XR_XIR_CALL_READY:XR_XIR_CALL_BAD_ARGUMENT;
}
static XrXirCallStatus task_aggregate_input_owned(XrXirTaskExecutor *executor,XirTask *task,XrXirCallView *view) {
    if(task->object.arena!=executor->config.task_arena||task->object.domain!=executor->config.domain||
        task->generation!=executor->generation||!task->identity||task->identity>executor->next_identity)return XR_XIR_CALL_BAD_STATE;
    if(task->aggregation&&!task->aggregation->kind)return XR_XIR_CALL_BAD_STATE;
    if(atomic_load_explicit(&task->state,memory_order_acquire)==XIR_TASK_TERMINAL)
        return task->terminal_order?XR_XIR_CALL_READY:XR_XIR_CALL_BAD_STATE;
    if(task->call==view->activation)return XR_XIR_CALL_BAD_STATE;
    if(task->aggregation) {
        XirTaskGroup *group=task->aggregation;
        return group->kind&&group->executor==executor&&group->completion==task&&!group->published?
            XR_XIR_CALL_READY:XR_XIR_CALL_BAD_STATE;
    }
    for(XirTask *actual=executor->active;actual;actual=actual->active_next) {
        if(!task_pending_member_work(xr_xir_call_admission(view),1))return XR_XIR_CALL_LIMIT;
        if(actual==task)return task->executor==executor&&task->call&&task->activation.call==task->call&&
            task->activation.task==task&&task->activation.ticket==task->identity?XR_XIR_CALL_READY:XR_XIR_CALL_BAD_STATE;
    }
    return XR_XIR_CALL_BAD_STATE;
}
static void task_aggregate_discard(XirTaskGroup *group,XirTask *task,uint64_t retained) {
    for(uint64_t a=0;a<retained;++a) {
        XrXirValue value=task_core_task_value(group->first[a].task);xr_xir_value_drop(&value);
    }
    if(group->first)xr_xir_domain_metadata_deallocate(group->executor->config.domain,group->first,
        group->count*sizeof(XirTaskSubscription));
    if(task){XrXirValue value=task_core_task_value(task);xr_xir_value_drop(&value);}
    xr_xir_domain_metadata_deallocate(group->executor->config.domain,group,sizeof(*group));
}
XR_FUNC XrXirCallStatus xir_task_aggregate_begin(XrXirTaskExecutor *executor,
    XrXirCallView *view,const XrXirValue *array,XrXirType result,XirTaskAggregateKind kind,XrXirValue *output) {
    bool child=false;
    if(!executor||!view||!array||!task_core_empty_value(output)||
        !xr_xir_task_executor_view_member(executor,view,&child)||task_executor_observing(executor)||
        executor->stopping)return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirValueAdmission *admission=xr_xir_call_admission(view);
    if(!admission||admission->domain!=executor->config.domain||admission->arena!=executor->config.task_arena||
        !xr_xir_value_argument(array,admission->arena,(XrXirType)array->type))return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirCallStatus status=task_aggregate_shape(executor,array,result,kind);
    if(status!=XR_XIR_CALL_READY)return status;
    int64_t length=0;XrXirValueStatus value=xr_xir_array_len(array,admission,&length);
    if(value!=XR_XIR_VALUE_OK)return task_core_value_status(value);
    if(!length&&kind==XIR_TASK_AGGREGATE_FIRST)return XR_XIR_CALL_UNSUPPORTED;
    if(length<0||(uint64_t)length>SIZE_MAX/sizeof(XirTaskSubscription)||
        (uint64_t)length>SIZE_MAX/sizeof(XrXirValue)||!task_terminal_available(executor)||
        executor->next_identity==UINT64_MAX||!task_pending_member_work(admission,(uint64_t)length*8u+4u))
        return XR_XIR_CALL_LIMIT;
    xr_xir_value_graph_begin();
    XirTaskGroup *group=xr_xir_domain_metadata_allocate(executor->config.domain,sizeof(*group),&value);
    if(!group){xr_xir_value_graph_end();return task_core_value_status(value);}
    memset(group,0,sizeof(*group));group->executor=executor;group->kind=(uint32_t)kind;group->count=(uint64_t)length;
    uint64_t bytes=group->count*sizeof(XirTaskSubscription),retained=0;
    if(bytes)group->first=xr_xir_domain_metadata_allocate(executor->config.domain,bytes,&value);
    XirTask *task=NULL;if(value==XR_XIR_VALUE_OK)value=task_core_task_prepare(executor,result,&task);
    status=task_core_value_status(value);
    if(group->first)memset(group->first,0,(size_t)bytes);
    for(uint64_t a=0;status==XR_XIR_CALL_READY&&a<group->count;++a) {
        XrXirValue owned={0};XrXirFaultDetail fault={0};value=xr_xir_array_get(array,(int64_t)a,admission,&owned,&fault);
        if(value!=XR_XIR_VALUE_OK){status=task_core_value_status(value);break;}
        XirTask *member=(XirTask *)(uintptr_t)owned.payload;
        status=task_aggregate_input_owned(executor,member,view);
        if(status!=XR_XIR_CALL_READY){xr_xir_value_drop(&owned);break;}
        XirTaskSubscription *slot=group->first+a;
        slot->task=member;slot->group=group;slot->ordinal=a;slot->next_member=a+1<group->count?slot+1:NULL;
        ++retained;
    }
    if(status==XR_XIR_CALL_READY)status=xr_xir_call_execution_status(view);
    if(status!=XR_XIR_CALL_READY){task_aggregate_discard(group,task,retained);xr_xir_value_graph_end();return status;}
    task->generation=executor->generation;task->identity=++executor->next_identity;++executor->pending_terminals;
    task->executor=executor;task->aggregation=group;group->completion=task;
    atomic_store_explicit(&task->object.references,2,memory_order_relaxed);
    atomic_store_explicit(&task->state,XIR_TASK_PENDING,memory_order_release);
    for(XirTaskSubscription *member=group->first;member;member=member->next_member) {
        member->next_subscription=member->task->subscriptions;member->task->subscriptions=member;
        if(atomic_load_explicit(&member->task->state,memory_order_acquire)==XIR_TASK_TERMINAL) {
            member->completed=true;task_aggregate_member(group,member);
        } else ++group->pending;
    }
    group->sealed=true;group->next=executor->groups;executor->groups=group;
    task_group_complete(group);task_group_flush(executor);*output=task_core_task_value(task);
    xr_xir_value_graph_end();return XR_XIR_CALL_READY;
}
static XrXirCallStatus task_group_cancel_target(XrXirTaskExecutor *executor,XirTask *task,
    XrXirValueAdmission *admission,bool *handled) {
    *handled=false;if(!task->aggregation)return XR_XIR_CALL_READY;
    for(XirTaskGroup *group=executor->groups;group;group=group->next) {
        if(!task_pending_member_work(admission,1))return XR_XIR_CALL_LIMIT;
        if(group!=task->aggregation)continue;
        if(!group->kind||group->completion!=task||group->published)return XR_XIR_CALL_BAD_STATE;
        *handled=true;group->cancel_requested=true;task_group_queue(group);return XR_XIR_CALL_READY;
    }
    return XR_XIR_CALL_BAD_STATE;
}
