/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_instance_root_callback.inc.c - Independent owned callback relay lifetime
 */
struct XirRootCallback {
    XrXirInstance *root, *generation;
    XrXirTaskExecutor *executor;
    XrXirCall *call, *parent;
    XrXirValue function;
    XrXirCallAccounting accounting;
    uint64_t parent_epoch;
    uint32_t entry;
};
static XrXirAction instance_callback_resume(XrXirCallView *view) {
    const XirRootCallback *callback=view->environment;
    if (!callback || callback->call!=view->activation || callback->root!=view->instance ||
        !callback->generation || !view->state)
        return (XrXirAction){.kind=XR_XIR_ACTION_FAULT,.value={XR_XIR_I64,0,XR_XIR_CALL_BAD_STATE}};
    uint32_t *phase=view->state;
    if (!*phase) {
        *phase=1;
        return (XrXirAction){.kind=XR_XIR_ACTION_CALL,.callee=callback->entry,.value=callback->function};
    }
    if (*phase!=1 || view->inbox.status!=XR_XIR_CALL_RETURNED)
        return (XrXirAction){.kind=XR_XIR_ACTION_FAULT,.value={XR_XIR_I64,0,XR_XIR_CALL_BAD_STATE}};
    *phase=2;
    return (XrXirAction){.kind=XR_XIR_ACTION_RETURN,.value=view->inbox.value};
}
static void instance_callback_release(XrXirCallView *view,XrXirCallStatus reason) {
    (void)view;(void)reason;
    /* The independent Instance owner outlives every relay and callee frame. */
}
XR_FUNC bool xir_instance_callback_owns(const XrXirInstance *instance,
    const XrXirTaskExecutor *executor,const XrXirCall *call) {
    if (!instance || !executor || !call || !instance_owner_current(instance)) return false;
    const XrXirInstance *root=instance_root((XrXirInstance *)instance);
    const XirRootCallback *callback=root->callback;
    if (!callback || callback->root!=root || callback->executor!=executor || callback->call!=call ||
        !callback->generation || callback->generation!=instance_current((XrXirInstance *)root) ||
        callback->generation->call!=callback->parent || callback->generation->epoch!=callback->parent_epoch ||
        callback->generation->executor!=executor || callback->generation->domain!=root->domain) return false;
    /* Revoking new FunctionBinding admission does not revoke the stable owner
     * already linked to this exact executor/Call/Parent epoch. Its real cleanup
     * and retirement must remain authenticated after stop clears gate.instance. */
    const XrXirFunctionBinding *binding=xr_xir_function_binding(&callback->function);
    return binding && binding->owner && callback->generation->function_gate &&
        binding->release==function_gate_drop && binding->owner==callback->generation->function_gate &&
        callback->entry==binding->entry && callback->entry<callback->generation->program->entry_count &&
        callback->generation->function_gate->program==callback->generation->program &&
        (callback->generation->function_gate->instance==root ||
         (!callback->generation->function_gate->instance &&
          (root->stopping || callback->generation->stopping)));
}
XR_FUNC bool xir_instance_callback_active(const XrXirInstance *instance) {
    if (!instance || !instance_owner_current(instance)) return false;
    XrXirInstance *active=instance_current((XrXirInstance *)instance);
    return active && xir_task_callback_active(active->executor);
}
XR_FUNC XrXirCallStatus xir_instance_callback_retire(XrXirInstance *instance,XrXirCall *call) {
    if (!instance || !instance_owner_current(instance)) return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirInstance *root=instance_root(instance);
    XirRootCallback *callback=root->callback;
    if (!callback || callback->call!=call || !xir_instance_callback_owns(root,callback->executor,call))
        return XR_XIR_CALL_BAD_ARGUMENT;
    if (xir_task_callback_active(callback->executor) ||
        xr_xir_call_state(call)==XR_XIR_CALL_READY || xr_xir_call_state(call)==XR_XIR_CALL_SUSPENDED)
        return XR_XIR_CALL_BAD_STATE;
    XrXirCallStatus status=xr_xir_call_free(call);
    if (status==XR_XIR_CALL_BUSY) return status;
    root->callback=NULL;
    xr_xir_value_drop(&callback->function);
    xr_xir_domain_metadata_deallocate(root->domain,callback,sizeof(*callback));
    return status;
}
XR_FUNC XrXirCallStatus xir_instance_callback_begin(XrXirInstance *instance,
    const XrXirValue *function) {
    if (!instance || !function || !instance_owner_current(instance)) return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirInstance *root=instance_root(instance),*active=instance_current(root),*birth=NULL;
    if (instance_busy(root) || instance_busy(active)) return XR_XIR_CALL_BUSY;
    if (root->stopping || root->prepared_generation || root->callback || !active || !active->call ||
        active->stopping || active->state!=XR_XIR_INSTANCE_READY) return XR_XIR_CALL_BAD_STATE;
    bool eligible=false;
    XrXirCallStatus status=xir_call_signal_frontier(active->call,&eligible);
    if (status!=XR_XIR_CALL_READY || !eligible) return status==XR_XIR_CALL_READY?XR_XIR_CALL_BAD_STATE:status;
    status=instance_function_state(root,function,&birth);
    if (status!=XR_XIR_CALL_READY) return status;
    /* A sibling shares the active executor's sealed Task and metadata domain.
     * A received older generation needs a real receiving map before this role. */
    if (birth!=active) return XR_XIR_CALL_BAD_STATE;
    XrXirValueAdmission admission=instance_candidate_admission(active);
    instance_driving(active,true);
    uint32_t entry=0;
    status=resolve_function(active,function,&entry,&admission);
    const XrXirTypeNode *signature=status==XR_XIR_CALL_READY?
        xr_xir_callable_signature(active->program->types,(XrXirType)function->type):NULL;
    const XrXirFunctionBinding *binding=status==XR_XIR_CALL_READY?xr_xir_function_binding(function):NULL;
    if (status==XR_XIR_CALL_READY && (!signature || !binding || signature->parameter_count ||
        signature->result!=XR_XIR_UNIT || signature->element!=XR_XIR_NEVER ||
        (signature->flags&XR_XIR_CALLABLE_REST) ||
        binding->capture_count!=active->program->entries[entry].parameter_count ||
        !function_bound_admitted(active->program,entry,signature))) status=XR_XIR_CALL_BAD_ARGUMENT;
    XirRootCallback *callback=NULL;
    XrXirCallEntry *entries=NULL;
    uint64_t bytes=((uint64_t)active->program->entry_count+1)*sizeof(*entries);
    if (status==XR_XIR_CALL_READY) {
        callback=instance_metadata_allocate(root->domain,sizeof(*callback),&status);
        if (callback) {
            *callback=(XirRootCallback){.root=root,.generation=active,.executor=active->executor,
                .parent=active->call,.parent_epoch=active->epoch,.entry=entry};
            status=value_call_status(xr_xir_value_copy(function,&callback->function));
        }
    }
    if (status==XR_XIR_CALL_READY) entries=instance_metadata_allocate(root->domain,bytes,&status);
    if (status==XR_XIR_CALL_READY) {
        memcpy(entries,active->program->entries,(size_t)active->program->entry_count*sizeof(*entries));
        entries[active->program->entry_count]=(XrXirCallEntry){XR_XIR_CALL_ABI_VERSION,NULL,0,XR_XIR_UNIT,
            sizeof(uint32_t),instance_callback_resume,instance_callback_release,callback,0,0};
        XrXirCallConfig config;
        status=xr_xir_call_config_init(&config,sizeof(config));
        if (status==XR_XIR_CALL_READY) {
            config.entries=entries;config.entry_count=active->program->entry_count+1;config.instance=root;
            config.byte_limit=active->config.call_limit;config.poll_limit=active->config.poll_limit;
            config.depth_limit=active->config.depth_limit;config.accounting=&callback->accounting;
            config.output=active->config.output;config.time=active->config.time;config.admission=admission;
            XrXirCallRequest request={active->program->entry_count,NULL,0,instance_cell_role,active->program};
            status=xr_xir_call_new_budgeted(&config,&request,instance_budget(root),&callback->call);
        }
    }
    if (entries) xr_xir_domain_metadata_deallocate(root->domain,entries,bytes);
    if (status==XR_XIR_CALL_READY) {
        root->callback=callback;
        status=xir_task_callback_attach(active->executor,callback->call);
        if (status!=XR_XIR_CALL_READY) root->callback=NULL;
    }
    if (status!=XR_XIR_CALL_READY && callback) {
        if (callback->call) (void)xr_xir_call_free(callback->call);
        xr_xir_value_drop(&callback->function);
        xr_xir_domain_metadata_deallocate(root->domain,callback,sizeof(*callback));
    }
    instance_driving(active,false);
    return status;
}
XR_FUNC XrXirCallStatus xir_instance_host_wait_deadline(XrXirInstance *instance,
    uint64_t epoch,uint64_t wake,uint64_t now_ns,XirHostWaitDeadline *output) {
    if (!instance || !output || !instance_owner_current(instance)) return XR_XIR_CALL_BAD_ARGUMENT;
    if (instance_busy(instance)) return XR_XIR_CALL_BUSY;
    XrXirInstance *active=instance_current(instance);
    if (!active || !active->call || active->stopping || epoch!=active->epoch) return XR_XIR_CALL_BAD_STATE;
    return xir_task_host_wait_deadline(active->executor,epoch,wake,now_ns,output);
}
