/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_instance_debug_activation.inc.c - Exact execution-flow metadata at authentic normal ingress
 */
XR_FUNC XrXirCallStatus xir_instance_debug_activation(XrXirCallView *view,XirInstanceDebugActivation *output) {
    XrXirInstance *instance=NULL;XrXirCallStatus status=instance_class_view(view,&instance);
    if (status!=XR_XIR_CALL_READY) return status;
    if (!output || !xr_xir_call_admission(view) || view->phase!=XR_XIR_CALL_NORMAL ||
        instance->stopping || xr_xir_call_cleanup_active(view->activation)) return XR_XIR_CALL_BAD_STATE;
    bool child=false;
    if (!xr_xir_task_executor_view_member(instance->executor,view,&child) ||
        (!child && view->activation!=instance->call) || (child && view->activation==instance->call)) return XR_XIR_CALL_BAD_STATE;
    XrXirValueAdmission *admission=xr_xir_call_admission(view);
    if (admission->work<6 || !xr_xir_domain_work(instance->domain,6)) return XR_XIR_CALL_LIMIT;
    admission->work-=6;
    uint32_t driver=instance->program->entry_count;
    if (!child) {
        const XrXirCallEntry *entry=&instance->entries[driver];
        if (entry->resume!=initialization_resume || entry->release!=initialization_release ||
            entry->state_bytes!=sizeof(uint32_t) || entry->environment || entry->flags || entry->cleanup_owner)
            return XR_XIR_CALL_BAD_STATE;
    }
    *output=(XirInstanceDebugActivation){child,child ? UINT32_MAX : driver,instance->epoch};return XR_XIR_CALL_READY;
}
