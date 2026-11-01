/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_task_group.inc.c - Lexical membership and terminal-driven group joins
 *
 * KEY CONCEPT:
 *   Registration owns the member before it enters the FIFO. A sealed group
 *   publishes only after every member has completed cleanup and its terminal.
 */
struct XirTaskSubscription {
    XirTaskGroup *group;
    XirTask *task;
    XirTaskSubscription *next_member,*next_subscription;
    uint64_t ordinal;
    bool completed;
};
struct XirTaskGroup {
    XrXirTaskExecutor *executor;
    XrXirCall *owner;
    uint64_t frame,count,pending;
    XirTask *completion;
    XirTaskSubscription *first,*last,*first_error,*first_panic;
    XirTaskGroup *next;
    XrXirCallStatus fault;
    bool isolated,sealed,cancelling,published;
};
static XrXirCallStatus task_group_owner(XrXirTaskExecutor *executor,XrXirCallView *view,
    const XrXirValue *value,XirTaskGroup **output) {
    bool child=false;uint64_t frame=0;
    const XrXirTypeNode *node=executor&&value?
        xr_xir_type_node(executor->config.program->types,(XrXirType)value->type):NULL;
    if(!task_executor_owner_current(executor)||!executor||!view||!value||
        !xr_xir_task_executor_view_member(executor,view,&child)||task_executor_observing(executor)||
        !node||node->kind!=XR_XIR_TYPE_TASK||
        !xr_xir_value_argument(value,executor->config.task_arena,(XrXirType)value->type))return XR_XIR_CALL_BAD_ARGUMENT;
    XirTask *task=(XirTask *)(uintptr_t)value->payload;XirTaskGroup *group=task->aggregation;
    XrXirCallStatus status=xir_call_scope_frame(view,&frame);
    if(status!=XR_XIR_CALL_READY)return status;
    if(!group||group->executor!=executor||group->completion!=task||group->owner!=view->activation||
        group->frame!=frame||task->generation!=executor->generation)return XR_XIR_CALL_BAD_STATE;
    *output=group;return XR_XIR_CALL_READY;
}
static void task_group_cancel_members(XirTaskGroup *group) {
    if(group->cancelling)return;
    group->cancelling=true;XrXirTaskExecutor *executor=group->executor;
    /* Registration prepays bounded cancellation and completion bookkeeping. */
    for(XirTaskSubscription *member=group->first;member;member=member->next_member) {
        XirTask *task=member->task;if(member->completed||!task->call)continue;
        XrXirCallStatus status=xr_xir_call_request_cancel(task->call);
        XR_CHECK(status==XR_XIR_CALL_CANCEL_REQUESTED||status==XR_XIR_CALL_BAD_STATE,
            "a lexical group requests cancellation only from its live owned members");
        if(!task->activation.queued&&executor->current!=&task->activation&&
            xr_xir_call_state(task->call)!=XR_XIR_CALL_SUSPENDED)task_core_enqueue(executor,&task->activation);
    }
}
static void task_group_complete(XirTaskGroup *group) {
    if(!group->sealed||group->pending||group->published)return;
    XrXirCallResult result={0};XirTaskSubscription *winner=group->first_panic?group->first_panic:group->first_error;
    if(group->fault!=XR_XIR_CALL_READY)result.status=group->fault;
    else if(winner) {
        XrXirValueStatus status=xr_xir_call_result_copy(&winner->task->outcome,&result);
        if(status!=XR_XIR_VALUE_OK)result.status=task_core_value_status(status);
    } else result.status=XR_XIR_CALL_RETURNED;
    XirTask *completion=group->completion;group->published=true;completion->executor=NULL;
    xr_xir_call_result_move(&result,&completion->outcome);
    atomic_store_explicit(&completion->state,XIR_TASK_TERMINAL,memory_order_release);
    while(completion->waiter_head)task_core_complete_wait(group->executor,completion->waiter_head);
}
static void task_group_notify(XrXirTaskExecutor *executor,XirTask *task) {
    for(XirTaskSubscription *member=task->subscriptions;member;member=member->next_subscription) {
        XirTaskGroup *group=member->group;
        XR_CHECK(group->executor==executor&&!member->completed&&group->pending,
            "each accepted member publishes exactly once to its original group");
        member->completed=true;--group->pending;XrXirCallStatus status=task->outcome.status;
        if(xr_xir_call_panic_status(status)) {if(!group->first_panic)group->first_panic=member;}
        else if(status==XR_XIR_CALL_THROWN) {if(!group->first_error)group->first_error=member;}
        else if(status!=XR_XIR_CALL_RETURNED&&status!=XR_XIR_CALL_CANCELLED&&group->fault==XR_XIR_CALL_READY)
            group->fault=status;
        if(!group->isolated&&(xr_xir_call_panic_status(status)||status==XR_XIR_CALL_THROWN))
            task_group_cancel_members(group);
        task_group_complete(group);
    }
}
static void task_group_member_commit(XirTaskSubscription *member,XirTask *task) {
    XirTaskGroup *group=member->group;
    XR_CHECK(!group->sealed&&group->count<UINT64_MAX,
        "a prepared lexical member commits before FIFO publication");
    member->task=task;member->ordinal=group->count++;
    member->next_subscription=task->subscriptions;task->subscriptions=member;
    if(group->last)group->last->next_member=member;else group->first=member;
    group->last=member;++group->pending;
    if(group->cancelling)XR_CHECK(xr_xir_call_request_cancel(task->call)==XR_XIR_CALL_CANCEL_REQUESTED,
        "a new member of a cancelling group is cancelled before it can run");
}
XR_FUNC XrXirCallStatus xir_task_scope_begin(XrXirTaskExecutor *executor,XrXirCallView *view,
    XrXirType type,uint32_t mode,XrXirValue *output) {
    bool child=false;uint64_t frame=0;
    if(!task_executor_owner_current(executor)||!executor||!view||!task_core_empty_value(output)||
        (mode!=0&&mode!=2)||!xr_xir_task_executor_view_member(executor,view,&child)||
        task_executor_observing(executor)||executor->stopping)return XR_XIR_CALL_BAD_ARGUMENT;
    const XrXirTypeNode *node=xr_xir_type_node(executor->config.program->types,type);
    if(!node||node->kind!=XR_XIR_TYPE_TASK||node->parameter_span||node->element!=XR_XIR_UNIT)
        return XR_XIR_CALL_BAD_ARGUMENT;
    if(node->result!=XR_XIR_NEVER)return XR_XIR_CALL_UNSUPPORTED;
    XrXirCallStatus status=xir_call_scope_frame(view,&frame);
    if(status!=XR_XIR_CALL_READY)return status;
    if(executor->next_identity==UINT64_MAX||!task_pending_member_work(xr_xir_call_admission(view),4))
        return XR_XIR_CALL_LIMIT;
    XrXirValueStatus allocation=XR_XIR_VALUE_OK;
    XirTaskGroup *group=xr_xir_domain_metadata_allocate(executor->config.domain,sizeof(*group),&allocation);
    if(!group)return task_core_value_status(allocation);
    memset(group,0,sizeof(*group));XirTask *task=NULL;
    allocation=task_core_task_prepare(executor,type,&task);
    if(allocation!=XR_XIR_VALUE_OK) {
        xr_xir_domain_metadata_deallocate(executor->config.domain,group,sizeof(*group));
        return task_core_value_status(allocation);
    }
    group->executor=executor;group->owner=view->activation;group->frame=frame;
    group->completion=task;group->isolated=mode==2;group->next=executor->groups;
    task->generation=executor->generation;task->identity=++executor->next_identity;
    task->executor=executor;task->aggregation=group;atomic_store_explicit(&task->object.references,2,memory_order_relaxed);
    atomic_store_explicit(&task->state,XIR_TASK_PENDING,memory_order_release);
    executor->groups=group;*output=task_core_task_value(task);return XR_XIR_CALL_READY;
}
XR_FUNC XrXirCallStatus xir_task_spawn_scoped(XrXirTaskExecutor *executor,XrXirCallView *view,
    const XrXirValue *scope,XrXirType type,const XrXirCallRequest *request,XrXirValue *output) {
    XirTaskGroup *group=NULL;
    XrXirCallStatus status=task_group_owner(executor,view,scope,&group);
    if(status!=XR_XIR_CALL_READY)return status;
    if(!request||!task_core_empty_value(output)||group->sealed)return XR_XIR_CALL_BAD_STATE;
    const XrXirTypeNode *node=xr_xir_type_node(executor->config.program->types,type);
    if(!node||node->kind!=XR_XIR_TYPE_TASK||node->result!=XR_XIR_NEVER)return XR_XIR_CALL_UNSUPPORTED;
    if(group->count==UINT64_MAX||!task_pending_member_work(xr_xir_call_admission(view),4))return XR_XIR_CALL_LIMIT;
    XrXirValueStatus allocation=XR_XIR_VALUE_OK;
    XirTaskSubscription *member=xr_xir_domain_metadata_allocate(executor->config.domain,sizeof(*member),&allocation);
    if(!member)return task_core_value_status(allocation);
    memset(member,0,sizeof(*member));member->group=group;
    xr_xir_value_graph_begin();
    status=task_core_spawn_inner(executor,view,type,request,output,member);
    xr_xir_value_graph_end();
    if(status!=XR_XIR_CALL_READY)xr_xir_domain_metadata_deallocate(executor->config.domain,member,sizeof(*member));
    return status;
}
XR_FUNC XrXirCallStatus xir_task_scope_seal(XrXirTaskExecutor *executor,XrXirCallView *view,
    const XrXirValue *scope,bool abandon) {
    XirTaskGroup *group=NULL;XrXirCallStatus status=task_group_owner(executor,view,scope,&group);
    if(status!=XR_XIR_CALL_READY)return status;
    if(group->sealed)return XR_XIR_CALL_BAD_STATE;
    group->sealed=true;if(abandon)task_group_cancel_members(group);
    task_group_complete(group);return XR_XIR_CALL_READY;
}
static void task_group_stop(XrXirTaskExecutor *executor) {
    for(XirTaskGroup *group=executor->groups;group;group=group->next) {
        group->sealed=true;task_group_cancel_members(group);task_group_complete(group);
    }
}
static void task_group_dispose_all(XrXirTaskExecutor *executor) {
    while(executor->groups) {
        XirTaskGroup *group=executor->groups;executor->groups=group->next;
        XR_CHECK(group->published&&!group->pending,"group release follows every member's terminal cleanup");
        for(XirTaskSubscription *member=group->first;member;) {
            XirTaskSubscription *next=member->next_member;XirTaskSubscription **link=&member->task->subscriptions;
            while(*link!=member) {XR_CHECK(*link,"a member keeps its original subscription");link=&(*link)->next_subscription;}
            *link=member->next_subscription;XrXirValue lease=task_core_task_value(member->task);xr_xir_value_drop(&lease);
            xr_xir_domain_metadata_deallocate(executor->config.domain,member,sizeof(*member));member=next;
        }
        group->completion->aggregation=NULL;XrXirValue lease=task_core_task_value(group->completion);
        xr_xir_value_drop(&lease);xr_xir_domain_metadata_deallocate(executor->config.domain,group,sizeof(*group));
    }
}
