/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_instance_function_producer.inc.c - Sealed current-frame producer selection
 *
 * KEY CONCEPT:
 *   Actual captures still pass complete admission; site identity grants no execution permission.
 */
static XrXirCallStatus instance_producer_site(XrXirInstance *instance,XrXirValueAdmission *admission,
    uint32_t caller,uint32_t instruction,const XrXirProgramFunctionRef **output) {
    const XrXirProgramPermissions *permissions=instance->program->permissions;
    if (!permissions || !permissions->effects || caller>=permissions->function_count ||
        permissions->function_count!=instance->program->entry_count) return XR_XIR_CALL_BAD_STATE;
    const XrXirProgramPermission *owner=&permissions->entries[caller];
    if (owner->reference_begin>permissions->reference_count ||
        owner->reference_count>permissions->reference_count-owner->reference_begin ||
        (owner->reference_count && !permissions->references)) return XR_XIR_CALL_BAD_STATE;
    uint64_t work=(uint64_t)owner->reference_count*5+2;
    if (!admission || admission->domain!=instance->domain || work>admission->work ||
        !xr_xir_domain_work(instance->domain,work)) return XR_XIR_CALL_LIMIT;
    admission->work-=work;
    const XrXirProgramFunctionRef *selected=NULL;
    for (uint32_t r=0;r<owner->reference_count;++r) {
        const XrXirProgramFunctionRef *site=&permissions->references[owner->reference_begin+r];
        if (site->instruction!=instruction) continue;
        if (selected) return XR_XIR_CALL_BAD_STATE;
        selected=site;
    }
    if (!selected || selected->entry>=instance->program->entry_count) return XR_XIR_CALL_BAD_ARGUMENT;
    *output=selected;return XR_XIR_CALL_READY;
}

XR_FUNC XrXirCallStatus xr_xir_instance_function_at(XrXirCallView *view,uint32_t instruction,
    const XrXirValue *captures,uint32_t count,XrXirValue *output) {
    XrXirInstance *instance=view_instance(view);
    if (!instance) return XR_XIR_CALL_BAD_STATE;
    if (!output || !value_unit(*output) || (count && !captures)) return XR_XIR_CALL_BAD_ARGUMENT;
    uint32_t caller=xr_xir_call_current_entry(view->activation);
    const XrXirProgramFunctionRef *selected=NULL;
    XrXirCallStatus status=instance_producer_site(instance,xr_xir_call_admission(view),caller,instruction,&selected);
    if (status!=XR_XIR_CALL_READY) return status;
    if (selected->captures!=count) return XR_XIR_CALL_BAD_ARGUMENT;
    const XrXirProgramPermissions *permissions=instance->program->permissions;
    const XirFunctionProducer producer={permissions->effects,caller,instruction,selected->site};
    const InstanceFunctionRequest request={selected->type,selected->entry,captures,count,&producer};
    return instance_function_create(view,&request,output);
}

XR_FUNC XrXirCallStatus xr_xir_instance_function_producer(XrXirCallView *view,
    const XrXirValue *function,XirEffectProducerView *output) {
    XrXirInstance *instance=view_instance(view);
    if (!instance) return XR_XIR_CALL_BAD_STATE;
    if (!function || !output) return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirValueAdmission *admission=xr_xir_call_admission(view);
    if (!admission || admission->domain!=instance->domain || admission->work<10 ||
        !xr_xir_domain_work(instance->domain,10)) return XR_XIR_CALL_LIMIT;
    admission->work-=10;
    if (!xr_xir_value_argument(function,instance->program->arena,(XrXirType)function->type))
        return XR_XIR_CALL_BAD_ARGUMENT;
    const XirFunctionProducer *producer=xr_xir_function_producer(function);
    const XrXirFunctionBinding *binding=xr_xir_function_binding(function);
    const XrXirProgramPermissions *permissions=instance->program->permissions;
    if (!producer || !permissions || producer->owner!=permissions->effects || !binding)
        return XR_XIR_CALL_BAD_ARGUMENT;
    const XrXirProgramFunctionRef *site=NULL;
    XrXirCallStatus status=instance_producer_site(instance,admission,producer->function,producer->instruction,&site);
    if (status!=XR_XIR_CALL_READY) return status;
    if (producer->site!=site->site || binding->entry!=site->entry ||
        binding->capture_count!=site->captures || function->type!=(uint32_t)site->type)
        return XR_XIR_CALL_BAD_ARGUMENT;
    status=admit_instance_value(admission,function,(XrXirType)function->type);
    if (status!=XR_XIR_CALL_READY) return status;
    uint64_t work=sizeof(*output);
    if (work>admission->work || !xr_xir_domain_work(instance->domain,work)) return XR_XIR_CALL_LIMIT;
    admission->work-=work;
    *output=(XirEffectProducerView){site->site,producer->function,producer->instruction,
        site->entry,site->captures,site->type};return XR_XIR_CALL_READY;
}
